// QingLong RV32: IF1 -> IF2 -> ID -> EX -> MEM -> WB.
// Harvard request/response ports; at most one outstanding transaction per port.
// Responses arrive >= 1 cycle after acceptance and remain valid until ready.
// Instruction requests are cancellable until accepted (not a direct AXI port).
module rv32_core #(
    parameter logic [31:0] RESET_PC = 32'h00000000,
    parameter integer BTB_BITS = 8,
    parameter bit PREDICT_ENABLE = 1,
    parameter bit DUAL_ISSUE = 1,
    parameter bit LOAD_BYPASS = 1,
    parameter bit LOAD_WORD_ONLY = 0,
    parameter bit EARLY_LSU = 1,
    parameter bit MULH_PIPELINE = 0
) (
    input logic clk, rst,
    output logic fence_i_o,
    output logic i_req_valid,
    input logic i_req_ready,
    output logic [31:0] i_req_addr,
    input logic i_rsp_valid,
    output logic i_rsp_ready,
    input logic [63:0] i_rsp_data,
    input logic [1:0] i_rsp_error,
    output logic d_req_valid,
    input logic d_req_ready,
    output logic [31:0] d_req_addr, d_req_wdata,
    output logic [3:0] d_req_wstrb,
    output logic d_req_write,
    input logic d_rsp_valid,
    output logic d_rsp_ready,
    input logic [31:0] d_rsp_data,
    input logic d_rsp_error,
    input logic irq_software, irq_timer, irq_external,
    output logic retire_valid,
    output logic [31:0] retire_pc, retire_insn, retire_data,
    output logic [4:0] retire_rd,
    output logic retire_second_valid,
    output logic [31:0] retire_second_pc, retire_second_insn, retire_second_data,
    output logic [4:0] retire_second_rd,
    output logic [31:0] dual_retire_count,
    output logic trap_valid,
    output logic [31:0] trap_pc, trap_cause, trap_value,
    output logic [63:0] cycle_count, instret_count,
    output logic [31:0] branch_count, mispredict_count, load_stall_count
);
    import rv32_pkg::*;
    // Two single-writer banks and a latest-writer bit per register allow LUT
    // RAM inference instead of a 2-write-port flip-flop array. Each bank may
    // be replicated by synthesis for its asynchronous read ports.
    logic [31:0] regs_lane0 [0:31], regs_lane1 [0:31];
    logic [31:0] regs_latest;
    logic [31:0] f_pc, prediction, prediction_second;
    wire prediction_sequential;
    logic q_valid, q_killed;
    logic [31:0] q_pc, q_pred, q_pred_second;
    logic q_has_second;
    logic id_valid, id_error;
    logic [31:0] id_pc, id_insn, id_pred;
    decode_t id_dec, id_dec_second, x_dec, x_dec_second;
    logic id_second_valid, id_second_error;
    logic [31:0] id_second_insn, id_second_pred;
    logic [2:0] fetch_count, fetch_remaining;
    logic [1:0] fetch_pop;
    logic [31:0] fetch_pc [0:3], fetch_insn [0:3], fetch_pred [0:3];
    logic fetch_error [0:3];
    // Small predecode travels with the FIFO. The issue/compaction decision
    // must not wait for the full instruction decoder's result multiplexer.
    logic [6:0] fetch_meta [0:3];
    // Each packet carries its relation to its predecessor. Moving the FIFO
    // moves these bits too; the first packet's predecessor is never examined.
    logic [2:0] fetch_matches[0:3];
    logic fetch_adjacent[0:3],fetch_sequential[0:3];
    function automatic [6:0] issue_metadata(input logic [31:0] insn);
        logic simple,writes,rs1,rs2,serial,control;
        logic [6:0] op,f7;
        logic [2:0] f3;
        begin
            op=insn[6:0];f7=insn[31:25];f3=insn[14:12];
            simple=0;writes=0;rs1=0;rs2=0;serial=0;control=0;
            case(op)
                7'h37,7'h17:begin simple=1;writes=1;end
                7'h13:begin
                    writes=1;rs1=1;
                    simple=(f3!=1 && f3!=5) || (f3==1 && f7==0) || (f3==5 && (f7==0 || f7==7'h20));
                end
                7'h33:begin
                    writes=1;rs1=1;rs2=1;
                    simple=f7==0 || (f7==7'h20 && (f3==0 || f3==5));
                end
                7'h03:begin writes=1;rs1=1;end
                7'h23:begin rs1=1;rs2=1;end
                7'h63:begin rs1=1;rs2=1;control=1;end
                7'h67:begin writes=1;rs1=1;control=1;end
                7'h6f:begin writes=1;control=1;end
                7'h0f,7'h73:serial=1;
                default:begin end
            endcase
            // Only the auxiliary-lane whitelist must prove legality here.
            // Full ID decode still handles illegal primary instructions; EX
            // exceptions cancel their younger lane before it can retire.
            issue_metadata={simple,1'b0,serial,control,writes,rs1,rs2};
        end
    endfunction
    function automatic [2:0] register_matches(input logic [31:0] older,younger);
        begin
            register_matches={
                older[11:7]!=0 && older[11:7]==younger[11:7],
                older[11:7]!=0 && older[11:7]==younger[24:20],
                older[11:7]!=0 && older[11:7]==younger[19:15]};
        end
    endfunction
    wire [1:0] fetch_last=fetch_count[1:0]-2'd1;
    wire [2:0] incoming_cross_matches=register_matches(fetch_insn[fetch_last],i_rsp_data[31:0]);
    wire [2:0] incoming_pair_matches=register_matches(i_rsp_data[31:0],i_rsp_data[63:32]);
    wire incoming_adjacent=fetch_pc[fetch_last]+32'd4==q_pc;
    wire incoming_sequential=q_pred==q_pc+32'd4;
    wire incoming_second_sequential=q_pred_second==q_pc+32'd8;
    wire [6:0] first_meta = fetch_meta[0], second_meta = fetch_meta[1];
    wire pair_dependency = (second_meta[1] && fetch_matches[1][0]) ||
         (second_meta[0] && fetch_matches[1][1]) || (second_meta[2] && fetch_matches[1][2]);
    integer fi;
    logic x_second_valid, m_second_valid, w_second_valid;
    logic [31:0] x_second_pc, x_second_insn, x_second_a, x_second_b;
    logic [31:0] m_second_insn, m_second_result, w_second_insn, w_second_result;
    logic [4:0] m_second_rd, w_second_rd;
    logic [31:0] a_second, b_second, alu_a_second, alu_b_second, alu_result_second;
    logic pair_eligible, pair_issue, pair_normal, pair_swap_candidate, pair_swapped, x_swapped, m_swapped;
    logic x_valid, x_error;
    logic [31:0] x_pc, x_insn, x_pred, x_a, x_b;
    logic m_valid, m_sent, m_exception;
    logic [31:0] m_pc, m_insn, m_addr, m_result, m_store;
    logic [31:0] m_cause, m_tval, m_csr_data;
    logic [11:0] m_csr_addr;
    logic [4:0] m_rd;
    logic [2:0] m_funct3;
    logic m_write_rd, m_load, m_store_en, m_csr_write, m_mret, m_serial;
    logic m_mul,m_late_alu;
    logic [4:0] m_alu_op;
    wire x_late_alu = x_dec.alu==ALU_MIN || x_dec.alu==ALU_MAX ||
        x_dec.alu==ALU_MINU || x_dec.alu==ALU_MAXU ||
        x_dec.alu==ALU_ROL || x_dec.alu==ALU_ROR ||
        x_dec.alu==ALU_CLZ || x_dec.alu==ALU_CTZ || x_dec.alu==ALU_CPOP;
    wire [31:0] late_alu_result;
    rv32_late_alu mem_alu (.op(m_alu_op), .a(m_result), .b(m_store), .result(late_alu_result));
    logic signed [32:0] m_mul_a, m_mul_b;
    // Four independent <=17x17 products avoid a four-APM cascade. MUL uses
    // the low product immediately; MULH variants register partial products
    // for an extra MEM cycle before their high-half sum.
    wire [31:0] product_ll = m_mul_a[15:0] * m_mul_b[15:0];
    wire signed [33:0] product_lh = $signed({1'b0,m_mul_a[15:0]}) *
                                    $signed(m_mul_b[32:16]);
    wire signed [33:0] product_hl = $signed(m_mul_a[32:16]) *
                                    $signed({1'b0,m_mul_b[15:0]});
    wire signed [33:0] product_hh = $signed(m_mul_a[32:16]) *
                                    $signed(m_mul_b[32:16]);
    wire [15:0] product_middle_low = product_ll[31:16] + product_lh[15:0] + product_hl[15:0];
    wire [31:0] product_low = {product_middle_low, product_ll[15:0]};
    logic mul_high_ready;
    logic mul_partial_ready;
    logic [31:0] mul_high_result;
    logic [15:0] partial_ll_high;
    logic signed [33:0] partial_lh, partial_hl, partial_hh;
    wire signed [34:0] partial_middle = $signed({19'b0,partial_ll_high}) +
        $signed({partial_lh[33],partial_lh}) + $signed({partial_hl[33],partial_hl});
    wire [31:0] product_high = partial_hh[31:0] + {{13{partial_middle[34]}},partial_middle[34:16]};
    logic w_valid, w_write_rd, w_serial;
    logic [31:0] w_pc, w_insn, w_result;
    logic [4:0] w_rd;
    logic [31:0] a, b, alu_a, alu_b, alu_result, branch_target, actual_next;
    wire [31:0] memory_address = a + x_dec.imm;
    wire memory_misaligned = (x_dec.funct3[1:0] == 1 && memory_address[0]) ||
                             (x_dec.funct3[1:0] == 2 && memory_address[1:0] != 0);
    logic taken, x_exception;
    logic [31:0] x_cause, x_tval, x_result;
    logic signed [32:0] mul_a, mul_b;
    wire load_bypass_ok=LOAD_BYPASS && (!LOAD_WORD_ONLY || m_funct3==2);
    logic hazard, x_go, x_ready, m_finish, m_ready, id_issue;
    logic mem_trap, irq_take, redirect, x_redirect;
    logic [31:0] redirect_pc;
    logic fetch_redirect, front_flush;
    wire [31:0] fetch_redirect_pc;
    logic [31:0] redirect_fallthrough,redirect_trap_vector;
    logic redirect_select_trap,redirect_select_taken,redirect_select_mret;
    // train_target is already captured on x_go; use it as the taken candidate.
    // Late branch comparison drives a one-bit register, never a 32-bit PC mux.
    assign fetch_redirect_pc = redirect_select_trap ? redirect_trap_vector :
        redirect_select_mret ? return_pc : redirect_select_taken ? train_target : redirect_fallthrough;
    logic i_accept, i_consume, d_accept;
    logic [31:0] load_shifted, mem_result;
    logic [31:0] csr_read_data, csr_source, csr_new;
    logic csr_legal, csr_readonly, csr_we;
    logic irq_pending;
    logic [31:0] irq_code, trap_vector, return_pc;
    logic div_busy, div_done, div_started, div_saved;
    logic [31:0] div_result, div_saved_result;
    logic div_start;
    // Compare register indices one stage before forwarding data is consumed.
    // Four operands: full rs1/rs2, auxiliary rs1/rs2. Valid/write/load checks
    // remain in EX. Hazard/response checks prevent unfinished data from retiring.
    logic [3:0] forward_m0, forward_m1, forward_w0, forward_w1;
    wire [4:0] decode_rs [0:3];
    wire [4:0] execute_rs [0:3];
    assign decode_rs[0] = pair_swapped ? id_dec_second.rs1 : id_dec.rs1;
    assign decode_rs[1] = pair_swapped ? id_dec_second.rs2 : id_dec.rs2;
    assign decode_rs[2] = pair_swapped ? id_dec.rs1 : id_dec_second.rs1;
    assign decode_rs[3] = pair_swapped ? id_dec.rs2 : id_dec_second.rs2;
    assign execute_rs[0] = x_dec.rs1;
    assign execute_rs[1] = x_dec.rs2;
    assign execute_rs[2] = x_dec_second.rs1;
    assign execute_rs[3] = x_dec_second.rs2;
    for (genvar operand = 0; operand < 4; operand = operand + 1) begin : forward_compare
        // When no instruction issues into an empty EX slot these bits are
        // don't-care. On a stall compare the held EX source indices instead.
        wire [4:0] next_rs = x_ready ? decode_rs[operand] : execute_rs[operand];
        wire [4:0] next_m0_rd = m_ready ? x_dec.rd : m_rd;
        wire [4:0] next_m1_rd = m_ready ? x_dec_second.rd : m_second_rd;
        always_ff @(posedge clk) begin
            if (rst) begin
                forward_m0[operand] <= 0; forward_m1[operand] <= 0;
                forward_w0[operand] <= 0; forward_w1[operand] <= 0;
            end else begin
                forward_m0[operand] <= next_rs != 0 && next_rs == next_m0_rd;
                forward_m1[operand] <= next_rs != 0 && next_rs == next_m1_rd;
                // WB is valid next cycle only when this MEM group finishes.
                forward_w0[operand] <= next_rs != 0 &&
                    (m_swapped ? next_rs == m_second_rd : next_rs == m_rd);
                forward_w1[operand] <= next_rs != 0 &&
                    (m_swapped ? (m_write_rd && next_rs == m_rd) : next_rs == m_second_rd);
            end
        end
    end
    // Register resolved branch training before the BTB/history RAM writes.
    // This removes the WB-forward -> compare -> counter-update timing chain.
    // Predictions may observe one older training state; EX still checks every
    // architectural next PC. FENCE.I drops the pending update as well.
    logic train_valid, train_conditional, train_taken, train_call, train_return;
    logic [31:0] train_pc, train_target;
    always_ff @(posedge clk) begin
        if (rst || (x_go && x_dec.fence_i)) train_valid <= 0;
        else train_valid <= x_go && !x_exception && !mem_trap &&
                            (x_dec.branch || x_dec.jal || x_dec.jalr);
        if (x_go) begin
            train_pc <= x_pc;
            train_target <= branch_target;
            train_conditional <= x_dec.branch;
            train_taken <= taken;
            train_call <= (x_dec.jal || x_dec.jalr) && (x_dec.rd == 1 || x_dec.rd == 5);
            train_return <= x_dec.jalr && x_dec.rd == 0 &&
                            (x_dec.rs1 == 1 || x_dec.rs1 == 5) && x_dec.imm == 0;
        end
    end

    function automatic [31:0] read_operand(input logic [4:0] index);
        begin
            if (index == 0) read_operand = 0;
            else if (w_second_valid && w_second_rd == index) read_operand = w_second_result;
            else if (w_valid && w_write_rd && w_rd == index) read_operand = w_result;
            else read_operand = regs_latest[index] ? regs_lane1[index] : regs_lane0[index];
        end
    endfunction

    rv32_decode decoder (.insn(id_insn), .decoded(id_dec));
    rv32_decode decoder_second (.insn(id_second_insn), .decoded(id_dec_second));
    rv32_alu #(.MINMAX_ENABLE(0), .LATE_OPS_ENABLE(0)) alu_full (.op(x_dec.alu), .a(alu_a), .b(alu_b), .result(alu_result));
    rv32_alu #(.EXTENSIONS(0)) alu_aux (
        .op(x_dec_second.alu), .a(alu_a_second), .b(alu_b_second), .result(alu_result_second));
    rv32_predictor #(.INDEX_BITS(BTB_BITS)) predictor (
        .clk(clk), .rst(rst), .clear(x_go && x_dec.fence_i),
        .pc(f_pc), .next_pc(prediction),
        .next_pc_sequential(prediction_sequential),
        .pc_second(f_pc + 32'd4), .next_pc_second(prediction_second),
        .update(train_valid), .update_pc(train_pc), .update_target(train_target),
        .update_conditional(train_conditional), .update_taken(train_taken),
        .update_call(train_call), .update_return(train_return)
    );
    rv32_divider divider (
        .clk(clk), .rst(rst), .cancel(mem_trap || irq_take),
        .start(div_start), .a(a), .b(b), .funct3(x_dec.funct3),
        .busy(div_busy), .done(div_done), .result(div_result)
    );
    rv32_csr csrs (
        .clk(clk), .rst(rst), .retire(w_valid), .retire_second(w_second_valid),
        .irq_software(irq_software), .irq_timer(irq_timer), .irq_external(irq_external),
        .read_addr(x_dec.csr_addr), .read_data(csr_read_data),
        .read_legal(csr_legal), .read_only(csr_readonly),
        .write_en(m_finish && !mem_trap && m_csr_write),
        .write_addr(m_csr_addr), .write_data(m_csr_data),
        .trap(trap_valid), .trap_interrupt(irq_take),
        .mret(m_finish && !mem_trap && m_mret),
        .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .irq_pending(irq_pending), .irq_cause(irq_code),
        .trap_vector(trap_vector), .return_pc(return_pc),
        .cycle_count(cycle_count), .instret_count(instret_count)
    );

    // Forwarding priority is newest (MEM), then WB. LOAD_BYPASS forwards a
    // response data in MEM; disabling it waits for the WB register. The data
    // mux may select an unfinished load, but hazard/m_ready keep EX stopped;
    // mem_trap cancels side effects on an error. Response qualification must
    // not be serialized before the ALU in this speculative data-only path.
    always_comb begin
        a = x_a; b = x_b;
        if (w_valid && w_write_rd) begin
            if (forward_w0[0]) a = w_result;
            if (forward_w0[1]) b = w_result;
        end
        if (w_second_valid) begin
            if (forward_w1[0]) a = w_second_result;
            if (forward_w1[1]) b = w_second_result;
        end
        if (m_valid && m_write_rd && !m_mul && !m_late_alu && (!m_load || load_bypass_ok)) begin
            if (forward_m0[0]) a = m_load ? (LOAD_WORD_ONLY ? d_rsp_data : mem_result) : m_result;
            if (forward_m0[1]) b = m_load ? (LOAD_WORD_ONLY ? d_rsp_data : mem_result) : m_result;
        end
        if (m_second_valid) begin
            if (forward_m1[0]) a = m_second_result;
            if (forward_m1[1]) b = m_second_result;
        end
        if (x_dec.rs1 == 0) a = 0;
        if (x_dec.rs2 == 0) b = 0;
        hazard = m_valid && m_write_rd && m_rd != 0 &&
                 ((m_load && !(load_bypass_ok && m_finish && !mem_trap)) || m_mul || m_late_alu) &&
                 ((x_dec.use_rs1 && forward_m0[0]) ||
                  (x_dec.use_rs2 && forward_m0[1]) ||
                  (x_second_valid && x_dec_second.use_rs1 && forward_m0[2]) ||
                  (x_second_valid && x_dec_second.use_rs2 && forward_m0[3]));
    end
    always_comb begin
        alu_a = x_dec.src_pc ? x_pc : a;
        alu_b = x_dec.src_imm ? x_dec.imm : b;
        taken = x_dec.jal || x_dec.jalr;
        if (x_dec.branch) begin
            case (x_dec.funct3)
                0: taken = a == b;
                1: taken = a != b;
                4: taken = $signed(a) < $signed(b);
                5: taken = $signed(a) >= $signed(b);
                6: taken = a < b;
                7: taken = a >= b;
                default: taken = 0;
            endcase
        end
        branch_target = x_dec.jalr ? ((a + x_dec.imm) & 32'hfffffffe) : x_pc + x_dec.imm;
        actual_next = taken ? branch_target : x_pc + 32'd4;
        if (x_dec.mret) actual_next = return_pc;
        csr_source = x_dec.csr_imm ? {27'b0, x_dec.rs1} : a;
        csr_we = x_dec.csr && (x_dec.funct3[1:0] == 1 || x_dec.rs1 != 0);
        case (x_dec.funct3[1:0])
            1: csr_new = csr_source;
            2: csr_new = csr_read_data | csr_source;
            default: csr_new = csr_read_data & ~csr_source;
        endcase
        mul_a = $signed({((x_dec.funct3 == 1 || x_dec.funct3 == 2) && a[31]), a});
        mul_b = $signed({(x_dec.funct3 == 1 && b[31]), b});
        x_result = alu_result;
        if (x_dec.jal || x_dec.jalr) x_result = x_pc + 32'd4;
        if (x_dec.csr) x_result = csr_read_data;
        if (x_dec.div) x_result = div_saved ? div_saved_result : div_result;
        // Payload selection depends on instruction class, not the late
        // misalignment/taken decision. It is observed only on an exception.
        x_tval = (x_dec.load || x_dec.store) ? memory_address : branch_target;
        x_exception = 0; x_cause = 0;
        if (x_error) begin
            x_exception = 1; x_cause = 1; x_tval = x_pc;
        end else if (x_dec.illegal || (x_dec.csr && (!csr_legal || (csr_we && csr_readonly)))) begin
            x_exception = 1; x_cause = 2; x_tval = x_insn;
        end else if (x_dec.ebreak) begin
            x_exception = 1; x_cause = 3; x_tval = x_pc;
        end else if (x_dec.ecall) begin
            x_exception = 1; x_cause = 11; x_tval = 0;
        end else if (taken && branch_target[1:0] != 0) begin
            x_exception = 1; x_cause = 0;
        end else if ((x_dec.load || x_dec.store) && memory_misaligned) begin
            x_exception = 1; x_cause = x_dec.load ? 4 : 6;
        end
    end

    // Lane 1 is a second integer ALU. Branches, memory, M-extension and system
    // instructions use lane 0. Same-pair RAW/WAW is resolved by issuing singly.
    assign pair_normal = DUAL_ISSUE && id_second_valid &&
        !id_error && !id_second_error && !first_meta[5] && !first_meta[4] &&
        !first_meta[3] && second_meta[6] &&
        fetch_sequential[0] && fetch_sequential[1] &&
        !(first_meta[2] && pair_dependency);
    // Dynamically route a simple older ALU to the auxiliary lane, leaving the
    // full lane available to a younger load/store, multiply, divide or branch.
    // Retire in program order and retain the older ALU if the younger faults.
    assign pair_swap_candidate = DUAL_ISSUE && id_second_valid &&
        !id_error && !id_second_error && first_meta[6] &&
        !second_meta[5] && !second_meta[4] &&
        fetch_sequential[0] &&
        !pair_dependency;
    assign pair_swapped = pair_swap_candidate && !pair_normal;
    // Admission does not need the lane-priority decision in its timing path.
    assign pair_eligible = pair_normal || pair_swap_candidate;
    assign pair_issue = id_issue && pair_eligible;
    assign id_valid = fetch_count != 0;
    assign id_pc = fetch_pc[0];
    assign id_insn = fetch_insn[0];
    assign id_pred = fetch_pred[0];
    assign id_error = fetch_error[0];
    assign id_second_valid = fetch_count >= 2 && fetch_adjacent[1];
    assign id_second_insn = fetch_insn[1];
    assign id_second_pred = fetch_pred[1];
    assign id_second_error = fetch_error[1];
    assign fetch_pop = id_issue ? (pair_issue ? 2'd2 : 2'd1) : 2'd0;
    assign fetch_remaining = fetch_count - {1'b0, fetch_pop};
    always_comb begin
        a_second = x_second_a; b_second = x_second_b;
        if (w_valid && w_write_rd) begin
            if (forward_w0[2]) a_second = w_result;
            if (forward_w0[3]) b_second = w_result;
        end
        if (w_second_valid) begin
            if (forward_w1[2]) a_second = w_second_result;
            if (forward_w1[3]) b_second = w_second_result;
        end
        if (m_valid && m_write_rd && !m_mul && !m_late_alu && (!m_load || load_bypass_ok)) begin
            if (forward_m0[2]) a_second = m_load ? (LOAD_WORD_ONLY ? d_rsp_data : mem_result) : m_result;
            if (forward_m0[3]) b_second = m_load ? (LOAD_WORD_ONLY ? d_rsp_data : mem_result) : m_result;
        end
        if (m_second_valid) begin
            if (forward_m1[2]) a_second = m_second_result;
            if (forward_m1[3]) b_second = m_second_result;
        end
        if (x_dec_second.rs1 == 0) a_second = 0;
        if (x_dec_second.rs2 == 0) b_second = 0;
        alu_a_second = x_dec_second.src_pc ? x_second_pc : a_second;
        alu_b_second = x_dec_second.src_imm ? x_dec_second.imm : b_second;
    end

    always_comb begin
        d_rsp_ready = m_valid && m_sent && !rst;
        m_finish = m_valid && (m_exception || !(m_load || m_store_en) ||
                              (d_rsp_ready && d_rsp_valid)) &&
                              (!m_mul || m_funct3 == 0 || mul_high_ready);
        mem_trap = m_finish && (m_exception || ((m_load || m_store_en) && d_rsp_error));
        m_ready = !m_valid || m_finish;
        load_shifted = d_rsp_data >> {m_addr[1:0], 3'b0};
        mem_result = m_result;
        if(m_late_alu) mem_result=late_alu_result;
        if (m_mul) mem_result = (m_funct3 == 0) ? product_low :
                               (MULH_PIPELINE ? mul_high_result : product_high);
        if (m_load) begin
            case (m_funct3)
                0: mem_result = {{24{load_shifted[7]}}, load_shifted[7:0]};
                1: mem_result = {{16{load_shifted[15]}}, load_shifted[15:0]};
                4: mem_result = {24'b0, load_shifted[7:0]};
                5: mem_result = {16'b0, load_shifted[15:0]};
                default: mem_result = d_rsp_data; // LW is always word-aligned.
            endcase
        end
    end
    // Launch address generation in EX, receive the synchronous RAM result in
    // MEM. Only advance EX once its request is accepted; older faults suppress
    // the request before any store side effect. A previous response may finish
    // on the same edge that the next request is accepted.
    always_comb begin
        if (EARLY_LSU) begin
            d_req_valid = x_valid && (x_dec.load || x_dec.store) &&
                !x_error && !x_dec.illegal && !memory_misaligned &&
                !hazard && m_ready && !mem_trap && !irq_take && !rst;
            d_req_addr = {memory_address[31:2], 2'b0};
            d_req_write = x_dec.store;
            d_req_wdata = x_dec.store ? b << {memory_address[1:0], 3'b0} : 32'b0;
            case (x_dec.funct3[1:0])
                0: d_req_wstrb = 4'b0001 << memory_address[1:0];
                1: d_req_wstrb = 4'b0011 << memory_address[1:0];
                default: d_req_wstrb = 4'b1111;
            endcase
        end else begin
            d_req_valid = m_valid && (m_load || m_store_en) && !m_exception && !m_sent && !rst;
            d_req_addr = {m_addr[31:2], 2'b0};
            d_req_write = m_store_en;
            d_req_wdata = m_store << {m_addr[1:0], 3'b0};
            case (m_funct3[1:0])
                0: d_req_wstrb = 4'b0001 << m_addr[1:0];
                1: d_req_wstrb = 4'b0011 << m_addr[1:0];
                default: d_req_wstrb = 4'b1111;
            endcase
        end
    end
    assign d_accept = d_req_valid && d_req_ready;
    assign div_start = x_valid && x_dec.div && !x_error && !x_dec.illegal && !hazard &&
                       !div_started && !mem_trap && !irq_take;
    assign x_go = x_valid && m_ready && !hazard &&
                  (!EARLY_LSU || !(x_dec.load || x_dec.store) || x_error ||
                   x_dec.illegal || memory_misaligned || d_req_ready) &&
                  (!x_dec.div || x_error || x_dec.illegal || div_done || div_saved);
    assign x_ready = !x_valid || x_go;
    assign irq_take = irq_pending && id_valid && !x_valid && !m_valid && !w_valid;
    assign id_issue = id_valid && x_ready && !irq_pending &&
                      !(first_meta[4] && (x_valid || m_valid || w_valid)) &&
                      !(x_valid && x_dec.serial) && !(m_valid && m_serial) &&
                      !(w_valid && w_serial);
    // Compare both outcomes in parallel; do not put a 32-bit equality chain
    // after the late forwarded branch condition.
    assign x_redirect = x_go && !x_exception &&
                        ((taken ? branch_target != x_pred : (x_pc + 32'd4) != x_pred) ||
                         x_dec.fence_i || x_dec.mret);
    assign fence_i_o = x_go && x_dec.fence_i && !mem_trap;
    assign trap_valid = mem_trap || irq_take;
    assign trap_pc = mem_trap ? m_pc : id_pc;
    assign trap_cause = mem_trap ? (m_exception ? m_cause : (m_load ? 32'd5 : 32'd7)) : irq_code;
    assign trap_value = mem_trap ? (m_exception ? m_tval : m_addr) : 32'b0;
    assign redirect = trap_valid || x_redirect;
    assign redirect_pc = trap_valid ? trap_vector : actual_next;
    assign front_flush = redirect || fetch_redirect;
    // Reserve FIFO slots without depending on this cycle's EX/memory-ready
    // chain. Requests issued on the redirect decision cycle are marked killed.
    assign i_rsp_ready = q_valid && (q_killed || fetch_redirect ||
                        fetch_count <= (q_has_second ? 3'd2 : 3'd3)) && !rst;
    assign i_consume = i_rsp_valid && i_rsp_ready;
    assign i_req_valid = (!q_valid || i_consume) && !fetch_redirect && !rst;
    assign i_req_addr = f_pc;
    assign i_accept = i_req_valid && i_req_ready;
    assign retire_valid = w_valid;
    assign retire_pc = w_pc;
    assign retire_insn = w_insn;
    assign retire_rd = w_write_rd ? w_rd : 5'b0;
    assign retire_data = (w_write_rd && w_rd != 0) ? w_result : 32'b0;
    assign retire_second_valid = w_second_valid;
    assign retire_second_pc = w_pc + 32'd4;
    assign retire_second_insn = w_second_insn;
    assign retire_second_rd = w_second_rd;
    assign retire_second_data = (w_second_rd != 0) ? w_second_result : 32'b0;

    always_ff @(posedge clk) begin
        if (rst) begin
            f_pc <= RESET_PC; q_valid <= 0; q_killed <= 0; q_pc <= 0; q_pred <= 0;
            q_pred_second <= 0; q_has_second <= 0;
            fetch_count <= 0;
            fetch_redirect <= 0; redirect_fallthrough<=0;redirect_trap_vector<=0;
            redirect_select_trap<=0;redirect_select_taken<=0;redirect_select_mret<=0;
            x_valid <= 0; x_dec <= '0; x_pc <= 0; x_insn <= 0; x_pred <= 0;
            x_second_valid <= 0; x_dec_second <= '0;
            x_swapped <= 0; m_swapped <= 0;
            x_second_pc <= 0; x_second_insn <= 0; x_second_a <= 0; x_second_b <= 0;
            x_error <= 0; x_a <= 0; x_b <= 0;
            m_valid <= 0; m_sent <= 0; m_exception <= 0;
            m_pc <= 0; m_insn <= 0; m_addr <= 0; m_result <= 0; m_store <= 0;
            m_cause <= 0; m_tval <= 0; m_csr_data <= 0; m_csr_addr <= 0;
            m_rd <= 0; m_funct3 <= 0; m_write_rd <= 0; m_load <= 0; m_store_en <= 0;
            m_csr_write <= 0; m_mret <= 0; m_serial <= 0; m_mul <= 0; m_late_alu <= 0; m_alu_op <= 0;
            m_mul_a <= 0; m_mul_b <= 0;
            mul_high_ready <= 0; partial_ll_high <= 0;
            mul_partial_ready <= 0; mul_high_result <= 0;
            partial_lh <= 0; partial_hl <= 0; partial_hh <= 0;
            m_second_valid <= 0; m_second_rd <= 0; m_second_result <= 0; m_second_insn <= 0;
            w_valid <= 0; w_write_rd <= 0; w_serial <= 0;
            w_pc <= 0; w_insn <= 0; w_result <= 0; w_rd <= 0;
            w_second_valid <= 0; w_second_rd <= 0; w_second_result <= 0; w_second_insn <= 0;
            div_started <= 0; div_saved <= 0; div_saved_result <= 0;
            branch_count <= 0; mispredict_count <= 0; load_stall_count <= 0; dual_retire_count <= 0;
            // No reset on x1..x31: ISA does not require their reset contents.
            // Startup initializes SP/GP. Avoid reset fanout across the register file.
        end else begin
            // In the optional MULH pipeline, validity controls observation,
            // not the DSP data clocks. Constant M/P enables avoid routing two
            // opposite phase enables from one dual-output LUT to the APM.
            if (MULH_PIPELINE) begin
                partial_ll_high <= product_ll[31:16];
                partial_lh <= product_lh;partial_hl <= product_hl;partial_hh <= product_hh;
                mul_high_result <= product_high;
            end
            if (m_ready) begin mul_high_ready <= 0; mul_partial_ready <= 0; end
            else if (m_valid && m_mul && m_funct3 != 0 && !mul_high_ready) begin
                if (MULH_PIPELINE && mul_partial_ready) begin
                    // Optional sum register isolates the MULH carry chain from
                    // result selection and WB. Low-half MUL latency is unchanged.
                    mul_high_ready <= 1;
                end else begin
                    if (!MULH_PIPELINE) begin
                        partial_ll_high <= product_ll[31:16];
                        partial_lh <= product_lh;partial_hl <= product_hl;partial_hh <= product_hh;
                    end
                    mul_partial_ready <= 1; mul_high_ready <= !MULH_PIPELINE;
                end
            end
            if (w_valid && w_write_rd && w_rd != 0) begin
                regs_lane0[w_rd] <= w_result; regs_latest[w_rd] <= 0;
            end
            if (w_second_valid && w_second_rd != 0) begin
                regs_lane1[w_second_rd] <= w_second_result; regs_latest[w_second_rd] <= 1;
            end
            // Register redirect data separately from its late enable, keeping
            // branch/exception decisions out of the fetch-PC arithmetic path.
            fetch_redirect <= redirect;
            redirect_fallthrough <= x_pc + 32'd4;
            redirect_trap_vector <= trap_vector;
            redirect_select_trap <= trap_valid;
            redirect_select_taken <= taken;
            redirect_select_mret <= x_dec.mret;
            if (fetch_redirect) f_pc <= fetch_redirect_pc;
            else if (i_accept) begin
                if (PREDICT_ENABLE && !prediction_sequential) f_pc <= prediction;
                else if (DUAL_ISSUE)
                    f_pc <= PREDICT_ENABLE ? prediction_second : f_pc + 32'd8;
                else f_pc <= f_pc + 32'd4;
            end
            if (i_consume) begin q_valid <= 0; q_killed <= 0; end
            if (front_flush && q_valid && !i_consume) q_killed <= 1;
            if (i_accept) begin
                q_valid <= 1; q_killed <= redirect; q_pc <= f_pc;
                q_pred <= PREDICT_ENABLE ? prediction : f_pc + 32'd4;
                q_pred_second <= PREDICT_ENABLE ? prediction_second : f_pc + 32'd8;
                q_has_second <= DUAL_ISSUE && (!PREDICT_ENABLE || prediction_sequential);
            end
            // Compact a four-entry instruction queue after consuming one or
            // two instructions. Pairing is independent of fetch-block borders.
            fetch_count <= fetch_remaining;
            for (fi = 0; fi < 4; fi = fi + 1) begin
                if (fetch_pop == 1 && fi < 3) begin
                    fetch_pc[fi] <= fetch_pc[fi+1]; fetch_insn[fi] <= fetch_insn[fi+1];
                    fetch_pred[fi] <= fetch_pred[fi+1]; fetch_error[fi] <= fetch_error[fi+1];
                    fetch_meta[fi] <= fetch_meta[fi+1];
                    fetch_matches[fi]<=fetch_matches[fi+1];
                    fetch_adjacent[fi]<=fetch_adjacent[fi+1];fetch_sequential[fi]<=fetch_sequential[fi+1];
                end else if (fetch_pop == 2 && fi < 2) begin
                    fetch_pc[fi] <= fetch_pc[fi+2]; fetch_insn[fi] <= fetch_insn[fi+2];
                    fetch_pred[fi] <= fetch_pred[fi+2]; fetch_error[fi] <= fetch_error[fi+2];
                    fetch_meta[fi] <= fetch_meta[fi+2];
                    fetch_matches[fi]<=fetch_matches[fi+2];
                    fetch_adjacent[fi]<=fetch_adjacent[fi+2];fetch_sequential[fi]<=fetch_sequential[fi+2];
                end
                // A flush invalidates count, so the data registers may safely
                // accept stale payloads. Avoid a late branch-to-array mux path.
                if (i_consume && !q_killed) begin
                    // Compare all pop counts in parallel instead of placing
                    // subtraction and increment before each payload write mux.
                    if ((fetch_pop == 0 && fetch_count == fi) ||
                        (fetch_pop == 1 && fetch_count == fi+1) ||
                        (fetch_pop == 2 && fetch_count == fi+2)) begin
                        fetch_pc[fi] <= q_pc; fetch_insn[fi] <= i_rsp_data[31:0];
                        fetch_pred[fi] <= q_pred; fetch_error[fi] <= i_rsp_error[0];
                        fetch_meta[fi] <= issue_metadata(i_rsp_data[31:0]);
                        fetch_matches[fi]<=incoming_cross_matches;
                        fetch_adjacent[fi]<=incoming_adjacent;fetch_sequential[fi]<=incoming_sequential;
                    end
                    if (q_has_second && fi > 0 &&
                        ((fetch_pop == 0 && fetch_count == fi-1) ||
                         (fetch_pop == 1 && fetch_count == fi) ||
                         (fetch_pop == 2 && fetch_count == fi+1))) begin
                        fetch_pc[fi] <= q_pc + 32'd4; fetch_insn[fi] <= i_rsp_data[63:32];
                        fetch_pred[fi] <= q_pred_second; fetch_error[fi] <= i_rsp_error[1];
                        fetch_meta[fi] <= issue_metadata(i_rsp_data[63:32]);
                        fetch_matches[fi]<=incoming_pair_matches;
                        fetch_adjacent[fi]<=1;fetch_sequential[fi]<=incoming_second_sequential;
                    end
                end
            end
            if (i_consume && !q_killed && !front_flush)
                fetch_count <= fetch_remaining + (q_has_second ? 3'd2 : 3'd1);
            if (front_flush) fetch_count <= 0;
            if (x_ready) begin
                x_valid <= id_issue && !redirect;
                x_second_valid <= pair_issue && !redirect;
                x_swapped <= pair_issue && pair_swapped;
                if (id_issue) begin
                    x_second_pc <= pair_swapped ? id_pc : fetch_pc[1];
                    if (pair_swapped) begin
                        x_dec <= id_dec_second; x_pc <= fetch_pc[1]; x_insn <= id_second_insn;
                        x_pred <= id_second_pred; x_error <= id_second_error;
                        x_a <= read_operand(id_dec_second.rs1); x_b <= read_operand(id_dec_second.rs2);
                        x_dec_second <= id_dec; x_second_insn <= id_insn;
                        x_second_a <= read_operand(id_dec.rs1); x_second_b <= read_operand(id_dec.rs2);
                    end else begin
                        x_dec <= id_dec; x_pc <= id_pc; x_insn <= id_insn;
                        x_pred <= id_pred; x_error <= id_error;
                        x_a <= read_operand(id_dec.rs1); x_b <= read_operand(id_dec.rs2);
                        x_dec_second <= id_dec_second; x_second_insn <= id_second_insn;
                        x_second_a <= read_operand(id_dec_second.rs1);
                        x_second_b <= read_operand(id_dec_second.rs2);
                    end
                end
                div_started <= 0; div_saved <= 0;
            end else if (x_valid) begin
                // Preserve a forwarded operand if a later stall outlives WB.
                x_a <= a; x_b <= b;
                x_second_a <= a_second; x_second_b <= b_second;
            end
            if (div_start) div_started <= 1;
            if (div_done && !x_go) begin div_saved <= 1; div_saved_result <= div_result; end
            if (mem_trap || irq_take) begin
                x_valid <= 0; x_second_valid <= 0; div_started <= 0; div_saved <= 0;
            end
            if (m_ready) begin
                m_valid <= x_go && !mem_trap && !irq_take;
                m_second_valid <= x_go && x_second_valid && (!x_exception || x_swapped) && !mem_trap && !irq_take;
                m_swapped <= x_swapped;
                m_sent <= EARLY_LSU && d_accept;
                if (x_go) begin
                    m_pc <= x_pc; m_insn <= x_insn; m_rd <= x_dec.rd;
                    // MIN/MAX reuse the existing operand payload registers.
                    // Dependent EX waits for WB rather than chaining two ALUs.
                    m_addr <= memory_address;
                    m_result <= x_late_alu ? alu_a : x_result;
                    m_store <= x_late_alu ? alu_b : b;
                    m_alu_op <= x_dec.alu;
                    m_late_alu<=x_late_alu;
                    m_funct3 <= x_dec.funct3;
                    m_write_rd <= x_dec.reg_write; m_load <= x_dec.load;
                    m_store_en <= x_dec.store; m_exception <= x_exception;
                    m_cause <= x_cause; m_tval <= x_tval;
                    m_csr_write <= csr_we; m_csr_addr <= x_dec.csr_addr; m_csr_data <= csr_new;
                    m_mret <= x_dec.mret; m_serial <= x_dec.serial;
                    m_mul <= x_dec.mul;
                    // EX registers forwarded operands for independent DSP
                    // products. High-half instructions use a second MEM cycle.
                    m_mul_a <= mul_a; m_mul_b <= mul_b;
                    m_second_insn <= x_second_insn; m_second_rd <= x_dec_second.rd;
                    m_second_result <= alu_result_second;
                end
            end else if (d_accept) m_sent <= 1;
            w_valid <= m_finish && (!mem_trap || (m_swapped && m_second_valid));
            w_second_valid <= m_finish && !mem_trap && m_second_valid;
            if (m_finish) begin
                w_pc <= m_swapped ? m_pc - 32'd4 : m_pc;
                w_insn <= m_swapped ? m_second_insn : m_insn;
                w_rd <= m_swapped ? m_second_rd : m_rd;
                w_write_rd <= m_swapped ? 1'b1 : m_write_rd;
                w_result <= m_swapped ? m_second_result : mem_result;
                w_serial <= m_serial;
                w_second_insn <= m_swapped ? m_insn : m_second_insn;
                w_second_rd <= m_swapped ? (m_write_rd ? m_rd : 5'b0) : m_second_rd;
                w_second_result <= m_swapped ? mem_result : m_second_result;
            end
            if (x_go && !mem_trap && !x_exception && (x_dec.branch || x_dec.jal || x_dec.jalr))
                branch_count <= branch_count + 32'd1;
            if (x_redirect && !mem_trap && (x_dec.branch || x_dec.jal || x_dec.jalr))
                mispredict_count <= mispredict_count + 32'd1;
            if (x_valid && hazard && m_load) load_stall_count <= load_stall_count + 32'd1;
            if (w_second_valid) dual_retire_count <= dual_retire_count + 32'd1;
        end
    end
endmodule
