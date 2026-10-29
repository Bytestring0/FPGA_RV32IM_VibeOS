// Source: rtl/rv32_pkg.sv
package rv32_pkg;
    // Named constants retain compatibility with the PDS 2022.2 ADS frontend,
    // which accepts packed structs but rejects SystemVerilog enum declarations.
    typedef logic [4:0] alu_op_t;
    localparam alu_op_t ALU_ADD=0, ALU_SUB=1, ALU_SLL=2, ALU_SLT=3,
        ALU_SLTU=4, ALU_XOR=5, ALU_SRL=6, ALU_SRA=7,
        ALU_OR=8, ALU_AND=9, ALU_COPY=10;
    localparam alu_op_t ALU_ANDN=11, ALU_ORN=12, ALU_XNOR=13,
        ALU_MIN=14, ALU_MAX=15, ALU_MINU=16, ALU_MAXU=17,
        ALU_ROL=18, ALU_ROR=19, ALU_CLZ=20, ALU_CTZ=21, ALU_CPOP=22,
        ALU_SEXTB=23, ALU_SEXTH=24, ALU_ZEXTH=25, ALU_ORCB=26, ALU_REV8=27,
        ALU_SH1ADD=28, ALU_SH2ADD=29, ALU_SH3ADD=30;
    typedef struct packed {
        logic illegal;
        logic [4:0] rs1, rs2, rd;
        logic use_rs1, use_rs2, reg_write;
        logic [31:0] imm;
        alu_op_t alu;
        logic src_pc, src_imm;
        logic branch, jal, jalr;
        logic load, store;
        logic [2:0] funct3;
        logic mul, div;
        logic csr, csr_imm;
        logic [11:0] csr_addr;
        logic ecall, ebreak, mret, fence_i, serial;
    } decode_t;
endpackage

// Source: rtl/rv32_decode.sv
module rv32_decode (
    input  logic [31:0] insn,
    // Flat port avoids a PDS 2022.2 ADS bug with package-qualified struct ports.
    // Must match the 87-bit packed rv32_pkg::decode_t.
    output wire [86:0] decoded
);
    import rv32_pkg::*;
    decode_t d;
    assign decoded = d;
    logic [6:0] op, f7;
    logic [2:0] f3;
    always_comb begin
        op = insn[6:0]; f3 = insn[14:12]; f7 = insn[31:25];
        d = '0;
        d.rs1 = insn[19:15]; d.rs2 = insn[24:20]; d.rd = insn[11:7];
        d.funct3 = f3;
        d.alu = ALU_ADD;
        case (op)
            7'h37, 7'h17: begin
                d.reg_write = 1; d.src_imm = 1;
                d.imm = {insn[31:12], 12'b0};
                d.src_pc = (op == 7'h17);
                d.alu = (op == 7'h37) ? ALU_COPY : ALU_ADD;
            end
            7'h6f: begin
                d.jal = 1; d.reg_write = 1;
                d.imm = {{11{insn[31]}}, insn[31], insn[19:12],
                         insn[20], insn[30:21], 1'b0};
            end
            7'h67: begin
                d.jalr = 1; d.reg_write = 1; d.use_rs1 = 1;
                d.imm = {{20{insn[31]}}, insn[31:20]};
                d.illegal = (f3 != 0);
            end
            7'h63: begin
                d.branch = 1; d.use_rs1 = 1; d.use_rs2 = 1;
                d.imm = {{19{insn[31]}}, insn[31], insn[7],
                         insn[30:25], insn[11:8], 1'b0};
                d.illegal = (f3 == 2 || f3 == 3);
            end
            7'h03: begin
                d.load = 1; d.reg_write = 1; d.use_rs1 = 1; d.src_imm = 1;
                d.imm = {{20{insn[31]}}, insn[31:20]};
                d.illegal = (f3 == 3 || f3 == 6 || f3 == 7);
            end
            7'h23: begin
                d.store = 1; d.use_rs1 = 1; d.use_rs2 = 1; d.src_imm = 1;
                d.imm = {{20{insn[31]}}, insn[31:25], insn[11:7]};
                d.illegal = (f3 > 2);
            end
            7'h13, 7'h33: begin
                d.reg_write = 1; d.use_rs1 = 1;
                d.use_rs2 = (op == 7'h33); d.src_imm = (op == 7'h13);
                d.imm = {{20{insn[31]}}, insn[31:20]};
                if (op == 7'h33 && f7 == 7'h01) begin
                    d.mul = !f3[2]; d.div = f3[2];
                end else begin
                    case (f3)
                        0: begin
                            d.alu = (op == 7'h33 && f7 == 7'h20) ? ALU_SUB : ALU_ADD;
                            if (op == 7'h33 && f7 != 0 && f7 != 7'h20) d.illegal = 1;
                        end
                        1: begin d.alu = ALU_SLL; if (f7 != 0) d.illegal = 1; end
                        2: d.alu = ALU_SLT;
                        3: d.alu = ALU_SLTU;
                        4: d.alu = ALU_XOR;
                        5: begin
                            d.alu = f7[5] ? ALU_SRA : ALU_SRL;
                            if (f7 != 0 && f7 != 7'h20) d.illegal = 1;
                        end
                        6: d.alu = ALU_OR;
                        7: d.alu = ALU_AND;
                    endcase
                    if (op == 7'h33 && f3 != 0 && f3 != 5 && f7 != 0) d.illegal = 1;
                end
                // RV32 Zba/Zbb encodings. Unknown/reserved encodings remain
                // illegal; these overrides do not enable other B subsets.
                if (op == 7'h33) begin
                    if (f7 == 7'h20 && (f3 == 4 || f3 == 6 || f3 == 7)) begin
                        d.illegal = 0;
                        case (f3)
                            4: d.alu = ALU_XNOR;
                            6: d.alu = ALU_ORN;
                            7: d.alu = ALU_ANDN;
                            default: begin end
                        endcase
                    end
                    if (f7 == 7'h05 && f3 >= 4) begin
                        d.illegal = 0;
                        case (f3)
                            4: d.alu = ALU_MIN;
                            5: d.alu = ALU_MINU;
                            6: d.alu = ALU_MAX;
                            7: d.alu = ALU_MAXU;
                            default: begin end
                        endcase
                    end
                    if (f7 == 7'h30 && (f3 == 1 || f3 == 5)) begin
                        d.illegal = 0; d.alu = f3 == 1 ? ALU_ROL : ALU_ROR;
                    end
                    if (f7 == 7'h04 && f3 == 4 && insn[24:20] == 0) begin
                        d.illegal = 0; d.alu = ALU_ZEXTH; d.use_rs2 = 0;
                    end
                    if (f7 == 7'h10 && (f3 == 2 || f3 == 4 || f3 == 6)) begin
                        d.illegal = 0;
                        case (f3)
                            2: d.alu = ALU_SH1ADD;
                            4: d.alu = ALU_SH2ADD;
                            6: d.alu = ALU_SH3ADD;
                            default: begin end
                        endcase
                    end
                end else begin
                    if (f3 == 1 && insn[31:25] == 7'h30) begin
                        case (insn[24:20])
                            0: begin d.alu = ALU_CLZ; d.illegal = 0; end
                            1: begin d.alu = ALU_CTZ; d.illegal = 0; end
                            2: begin d.alu = ALU_CPOP; d.illegal = 0; end
                            4: begin d.alu = ALU_SEXTB; d.illegal = 0; end
                            5: begin d.alu = ALU_SEXTH; d.illegal = 0; end
                            default: begin end
                        endcase
                    end
                    if (f3 == 5 && f7 == 7'h30) begin d.alu = ALU_ROR; d.illegal = 0; end
                    if (f3 == 5 && insn[31:20] == 12'h287) begin d.alu = ALU_ORCB; d.illegal = 0; end
                    if (f3 == 5 && insn[31:20] == 12'h698) begin d.alu = ALU_REV8; d.illegal = 0; end
                end
            end
            7'h0f: begin
                // FENCE is a full pipeline barrier; FENCE.I also flushes fetch/BTB.
                d.serial = 1; d.fence_i = (f3 == 1);
                d.illegal = (f3 != 0 && f3 != 1);
            end
            7'h73: begin
                d.serial = 1;
                if (f3 == 0) begin
                    case (insn)
                        32'h00000073: d.ecall = 1;
                        32'h00100073: d.ebreak = 1;
                        32'h30200073: d.mret = 1;
                        32'h10500073: begin end // WFI: legal implementation as NOP.
                        default: d.illegal = 1;
                    endcase
                end else if (f3 == 4) d.illegal = 1;
                else begin
                    d.csr = 1; d.reg_write = 1;
                    d.csr_imm = f3[2]; d.use_rs1 = !f3[2];
                    d.csr_addr = insn[31:20];
                end
            end
            default: d.illegal = 1;
        endcase
    end
endmodule

// Source: rtl/rv32_alu.sv
// Separate left/right shifters keep late forwarded operands off reversal muxes.
// Zba/Zbb are only instantiated in the full execution lane.
module rv32_alu #(parameter bit EXTENSIONS = 1) (
    input logic [4:0] op,
    input logic [31:0] a, b,
    output logic [31:0] result
);
    import rv32_pkg::*;
    wire rotating = EXTENSIONS && (op == ALU_ROL || op == ALU_ROR);
    wire [31:0] reverse_a;
    wire [31:0] shift_upper = rotating ? a : (op == ALU_SRA ? {32{a[31]}} : 32'b0);
    wire [63:0] shifted_right = {shift_upper, a} >> b[4:0];
    wire [63:0] shifted_left = {a, (rotating ? a : 32'b0)} << b[4:0];
    for (genvar k = 0; k < 32; k = k + 1) begin
        assign reverse_a[k] = a[31-k];
    end
    wire [31:0] add_a = !EXTENSIONS ? a : op == ALU_SH1ADD ? {a[30:0],1'b0} :
        op == ALU_SH2ADD ? {a[29:0],2'b0} : op == ALU_SH3ADD ? {a[28:0],3'b0} : a;
    function automatic [5:0] leading_zeros(input logic [31:0] v);
        logic [31:0] t;
        logic [5:0] n;
        begin
            t = v; n = 0;
            if (t[31:16] == 0) begin n = n + 6'd16; t = t << 16; end
            if (t[31:24] == 0) begin n = n + 6'd8; t = t << 8; end
            if (t[31:28] == 0) begin n = n + 6'd4; t = t << 4; end
            if (t[31:30] == 0) begin n = n + 6'd2; t = t << 2; end
            if (!t[31]) n = n + 6'd1;
            leading_zeros = v == 0 ? 6'd32 : n;
        end
    endfunction
    wire [2:0] pop4 [0:7];
    wire [3:0] pop8 [0:3];
    wire [4:0] pop16 [0:1];
    wire [5:0] pop32 = {1'b0,pop16[0]} + {1'b0,pop16[1]};
    for (genvar p = 0; p < 8; p = p + 1)
        assign pop4[p] = {2'b0,a[p*4]} + {2'b0,a[p*4+1]} + {2'b0,a[p*4+2]} + {2'b0,a[p*4+3]};
    for (genvar p = 0; p < 4; p = p + 1)
        assign pop8[p] = {1'b0,pop4[p*2]} + {1'b0,pop4[p*2+1]};
    for (genvar p = 0; p < 2; p = p + 1)
        assign pop16[p] = {1'b0,pop8[p*2]} + {1'b0,pop8[p*2+1]};
    always_comb begin
        result = 0;
        case (op)
            ALU_ADD: result = add_a + b;
            ALU_SUB: result = a - b;
            ALU_SLL: result = shifted_left[63:32];
            ALU_SRL, ALU_SRA: result = shifted_right[31:0];
            ALU_SLT: result = {31'b0,$signed(a) < $signed(b)};
            ALU_SLTU: result = {31'b0,a < b};
            ALU_XOR: result = a ^ b;
            ALU_OR: result = a | b;
            ALU_AND: result = a & b;
            ALU_COPY: result = b;
            // Keep a single parallel selection layer for base and Zb results.
            // EXTENSIONS is constant-folded for the auxiliary execution lane.
            ALU_ANDN: result = EXTENSIONS ? a & ~b : 0;
            ALU_ORN: result = EXTENSIONS ? a | ~b : 0;
            ALU_XNOR: result = EXTENSIONS ? ~(a ^ b) : 0;
            ALU_MIN: result = EXTENSIONS ? ($signed(a) < $signed(b) ? a : b) : 0;
            ALU_MAX: result = EXTENSIONS ? ($signed(a) > $signed(b) ? a : b) : 0;
            ALU_MINU: result = EXTENSIONS ? (a < b ? a : b) : 0;
            ALU_MAXU: result = EXTENSIONS ? (a > b ? a : b) : 0;
            ALU_ROL: result = EXTENSIONS ? shifted_left[63:32] : 0;
            ALU_ROR: result = EXTENSIONS ? shifted_right[31:0] : 0;
            ALU_CLZ, ALU_CTZ: result = EXTENSIONS ? {26'b0,leading_zeros(op == ALU_CTZ ? reverse_a : a)} : 0;
            ALU_CPOP: result = EXTENSIONS ? {26'b0,pop32} : 0;
            ALU_SEXTB: result = EXTENSIONS ? {{24{a[7]}},a[7:0]} : 0;
            ALU_SEXTH: result = EXTENSIONS ? {{16{a[15]}},a[15:0]} : 0;
            ALU_ZEXTH: result = EXTENSIONS ? {16'b0,a[15:0]} : 0;
            ALU_ORCB: result = EXTENSIONS ? {{8{|a[31:24]}},{8{|a[23:16]}},{8{|a[15:8]}},{8{|a[7:0]}}} : 0;
            ALU_REV8: result = EXTENSIONS ? {a[7:0],a[15:8],a[23:16],a[31:24]} : 0;
            ALU_SH1ADD, ALU_SH2ADD, ALU_SH3ADD: result = EXTENSIONS ? add_a + b : 0;
            default: result = 0;
        endcase
    end
endmodule

// Source: rtl/rv32_predictor.sv
// Tagged BTB, per-entry local history, banked two-bit direction counters, RAS.
// Independent single-writer banks preserve LUT-RAM inference in PDS ADS.
module rv32_predictor #(
    parameter integer INDEX_BITS = 6,
    parameter bit RAS_ENABLE = 1,
    parameter bit LOCAL_HISTORY = 1,
    parameter integer HISTORY_BITS = 3
) (
    input logic clk, rst, clear,
    input logic [31:0] pc, pc_second,
    output logic [31:0] next_pc, next_pc_second,
    input logic update,
    input logic [31:0] update_pc, update_target,
    input logic update_conditional, update_taken,
    input logic update_call, update_return
);
    localparam integer N = 1 << INDEX_BITS;
    logic [N-1:0] valid;
    logic [29-INDEX_BITS:0] tags [0:N-1];
    logic [31:0] targets [0:N-1];
    logic [1:0] counters [0:N-1];
    logic [HISTORY_BITS-1:0] history [0:N-1];
    wire [(1<<HISTORY_BITS)-1:0] local_direction, local_direction_second;
    wire direction = LOCAL_HISTORY ? local_direction[history[ri]] : counters[ri][1];
    wire direction_second = LOCAL_HISTORY ? local_direction_second[history[ri_second]] : counters[ri_second][1];
    logic conditional [0:N-1];
    logic is_return [0:N-1];
    logic [31:0] return_stack [0:15];
    logic [3:0] return_pointer;
    logic [4:0] return_count;
    wire [3:0] return_top = return_pointer - 4'd1;
    wire [INDEX_BITS-1:0] ri = pc[INDEX_BITS+1:2];
    wire [INDEX_BITS-1:0] ri_second = pc_second[INDEX_BITS+1:2];
    wire [INDEX_BITS-1:0] wi = update_pc[INDEX_BITS+1:2];
    generate for (genvar bank = 0; bank < (1<<HISTORY_BITS); bank = bank+1) begin : direction_bank
        localparam logic [HISTORY_BITS-1:0] BANK_ID = bank;
        logic [1:0] values [0:N-1];
        assign local_direction[bank] = values[ri][1];
        assign local_direction_second[bank] = values[ri_second][1];
        always_ff @(posedge clk) begin
            if (LOCAL_HISTORY && !rst && !clear && update) begin
                if (!valid[wi] || tags[wi] != update_pc[31:INDEX_BITS+2])
                    values[wi] <= update_taken ? 2'b10 : 2'b01;
                else if (update_conditional && history[wi] == BANK_ID) begin
                    if (update_taken && values[wi] != 3) values[wi] <= values[wi] + 2'd1;
                    else if (!update_taken && values[wi] != 0) values[wi] <= values[wi] - 2'd1;
                end
            end
        end
    end endgenerate
    always_comb begin
        next_pc = pc + 32'd4;
        if (valid[ri] && tags[ri] == pc[31:INDEX_BITS+2] &&
            (!conditional[ri] || direction))
            next_pc = (RAS_ENABLE && is_return[ri] && return_count != 0) ?
                      return_stack[return_top] : targets[ri];
        next_pc_second = pc_second + 32'd4;
        if (valid[ri_second] && tags[ri_second] == pc_second[31:INDEX_BITS+2] &&
            (!conditional[ri_second] || direction_second))
            next_pc_second = (RAS_ENABLE && is_return[ri_second] && return_count != 0) ?
                             return_stack[return_top] : targets[ri_second];
    end
    always_ff @(posedge clk) begin
        if (rst || clear) begin
            valid <= '0;
            return_pointer <= 0; return_count <= 0;
        end else if (update) begin
            valid[wi] <= 1;
            tags[wi] <= update_pc[31:INDEX_BITS+2];
            targets[wi] <= update_target;
            conditional[wi] <= update_conditional;
            is_return[wi] <= update_return;
            if (LOCAL_HISTORY) begin
                if (!valid[wi] || tags[wi] != update_pc[31:INDEX_BITS+2]) begin
                    history[wi] <= {{(HISTORY_BITS-1){1'b0}},update_taken};
                end else if (update_conditional) begin
                    history[wi] <= {history[wi][HISTORY_BITS-2:0],update_taken};
                end
            end
            // Execution-updated RAS: no speculative stack rollback is needed.
            // A very short call/return chain may still miss and is corrected
            // by the normal architectural next-PC check in EX.
            if (RAS_ENABLE && update_call) begin
                return_stack[return_pointer] <= update_pc + 32'd4;
                return_pointer <= return_pointer + 4'd1;
                if (return_count != 16) return_count <= return_count + 5'd1;
            end else if (RAS_ENABLE && update_return && return_count != 0) begin
                return_pointer <= return_pointer - 4'd1;
                return_count <= return_count - 5'd1;
            end
            if (!valid[wi] || tags[wi] != update_pc[31:INDEX_BITS+2])
                counters[wi] <= update_taken ? 2'b10 : 2'b01;
            else if (update_taken && counters[wi] != 3)
                counters[wi] <= counters[wi] + 2'd1;
            else if (!update_taken && counters[wi] != 0)
                counters[wi] <= counters[wi] - 2'd1;
        end
    end
endmodule

// Source: rtl/rv32_divider.sv
// Radix-2 restoring divider, no combinational "/" datapath.
// start is sampled when idle. done pulses once; result remains stable.
module rv32_divider (
    input logic clk, rst, cancel, start,
    input logic [31:0] a, b,
    input logic [2:0] funct3,
    output logic busy, done,
    output logic [31:0] result
);
    logic [31:0] quotient, divisor;
    logic [32:0] remainder, trial, difference;
    logic [5:0] count;
    logic negate_q, negate_r, want_rem, prepare, negate_divisor;
    logic [31:0] q_next, r_next;
    wire signed_op = !funct3[0];
    always_comb begin
        trial = {remainder[31:0], quotient[31]};
        difference = trial - {1'b0, divisor};
        q_next = {quotient[30:0], !difference[32]};
        r_next = difference[32] ? trial[31:0] : difference[31:0];
    end
    always_ff @(posedge clk) begin
        if (rst || cancel) begin
            busy <= 0; done <= 0; result <= 0;
            quotient <= 0; divisor <= 0; remainder <= 0; count <= 0;
            negate_q <= 0; negate_r <= 0; want_rem <= 0;
            prepare <= 0; negate_divisor <= 0;
        end else begin
            done <= 0;
            // Idle operand capture is unconditional; start only controls the
            // transaction state. Keep its late hazard/exception logic out of
            // the wide operand registers' clock-enable chain.
            if (!busy) begin quotient <= a; divisor <= b; end
            if (start && !busy) begin
                    // Capture raw operands before the absolute-value adders.
                    // Unsigned and nonnegative signed divides start directly.
                    // Zero/overflow operands follow the same iteration path,
                    // removing wide operand comparisons from the input enable.
                    prepare <= signed_op && (a[31] || b[31]);
                    negate_divisor <= signed_op && b[31];
                    remainder <= 0; count <= 0; busy <= 1;
                    negate_q <= signed_op && (a[31] ^ b[31]);
                    negate_r <= signed_op && a[31]; want_rem <= funct3[1];
            end else if (busy) begin
                if (prepare) begin
                    quotient <= negate_r ? -quotient : quotient;
                    divisor <= negate_divisor ? -divisor : divisor;
                    prepare <= 0;
                end else begin
                quotient <= q_next; remainder <= {1'b0, r_next};
                count <= count + 6'd1;
                if (count == 31) begin
                    busy <= 0; done <= 1;
                    // Restoring division naturally preserves the dividend as
                    // remainder for zero divisors. DIV[U] must return all ones,
                    // including negative signed dividends. Signed overflow
                    // naturally wraps to INT_MIN with remainder zero.
                    result <= want_rem ? (negate_r ? -r_next : r_next) :
                              divisor == 0 ? 32'hffffffff :
                              (negate_q ? -q_next : q_next);
                end
                end
            end
        end
    end
endmodule

// Source: rtl/rv32_csr.sv
// M-mode only. Interrupt inputs must be synchronous to clk.
module rv32_csr #(
    parameter logic [31:0] HART_ID = 0
) (
    input logic clk, rst, retire, retire_second,
    input logic irq_software, irq_timer, irq_external,
    input logic [11:0] read_addr,
    output logic [31:0] read_data,
    output logic read_legal, read_only,
    input logic write_en,
    input logic [11:0] write_addr,
    input logic [31:0] write_data,
    input logic trap, trap_interrupt, mret,
    input logic [31:0] trap_pc, trap_cause, trap_value,
    output logic irq_pending,
    output logic [31:0] irq_cause, trap_vector, return_pc,
    output logic [63:0] cycle_count, instret_count
);
    logic [31:0] mstatus, mie, mtvec, mscratch, mepc, mcause, mtval;
    logic [31:0] mip, inhibit;
    logic [31:0] enabled;
    always_comb begin
        mip = 0; mip[3] = irq_software; mip[7] = irq_timer; mip[11] = irq_external;
        enabled = mip & mie;
        irq_pending = mstatus[3] && |enabled;
        irq_cause = enabled[11] ? 32'd11 : enabled[3] ? 32'd3 : 32'd7;
    end
    always_comb begin
        trap_vector = {mtvec[31:2], 2'b0};
        if (trap_interrupt && mtvec[1:0] == 1)
            trap_vector = {mtvec[31:2], 2'b0} + {irq_cause[29:0], 2'b0};
        return_pc = mepc;
    end
    always_comb begin
        read_data = 0; read_legal = 1;
        read_only = (read_addr[11:10] == 2'b11);
        case (read_addr)
            12'h300: read_data = mstatus;
            12'h301: read_data = 32'h40001100; // RV32 I + M, fixed WARL.
            12'h304: read_data = mie;
            12'h305: read_data = mtvec;
            12'h306: read_data = 0; // No lower privilege modes.
            12'h310: read_data = 0; // Little-endian, no optional mstatush fields.
            12'h320: read_data = inhibit;
            12'h340: read_data = mscratch;
            12'h341: read_data = mepc;
            12'h342: read_data = mcause;
            12'h343: read_data = mtval;
            12'h344: read_data = mip;
            12'hb00, 12'hc00: read_data = cycle_count[31:0];
            12'hb80, 12'hc80: read_data = cycle_count[63:32];
            12'hb02, 12'hc02: read_data = instret_count[31:0];
            12'hb82, 12'hc82: read_data = instret_count[63:32];
            12'hf11, 12'hf12, 12'hf13: read_data = 0;
            12'hf14: read_data = HART_ID;
            default: read_legal = 0;
        endcase
    end
    always_ff @(posedge clk) begin
        if (rst) begin
            mstatus <= 32'h1800; mie <= 0; mtvec <= 0; mscratch <= 0;
            mepc <= 0; mcause <= 0; mtval <= 0; inhibit <= 0;
            cycle_count <= 0; instret_count <= 0;
        end else begin
            if (!inhibit[0]) cycle_count <= cycle_count + 64'd1;
            if ((retire || retire_second) && !inhibit[2])
                instret_count <= instret_count + {63'b0, retire} + {63'b0, retire_second};
            if (write_en) begin
                case (write_addr)
                    12'h300: mstatus <= (write_data & 32'h88) | 32'h1800;
                    12'h304: mie <= write_data & 32'h888;
                    12'h305: mtvec <= {write_data[31:2], 1'b0, write_data[1:0] == 1};
                    12'h320: inhibit <= write_data & 32'h5;
                    12'h340: mscratch <= write_data;
                    12'h341: mepc <= {write_data[31:2], 2'b0};
                    12'h342: mcause <= write_data;
                    12'h343: mtval <= write_data;
                    12'hb00: cycle_count <= {cycle_count[63:32], write_data};
                    12'hb80: cycle_count <= {write_data, cycle_count[31:0]};
                    12'hb02: instret_count <= {instret_count[63:32], write_data};
                    12'hb82: instret_count <= {write_data, instret_count[31:0]};
                    default: begin end // Fixed WARL fields ignore writes.
                endcase
            end
            if (trap) begin
                mepc <= {trap_pc[31:2], 2'b0};
                mcause <= {trap_interrupt, trap_cause[30:0]};
                mtval <= trap_value;
                mstatus[7] <= mstatus[3]; mstatus[3] <= 0;
            end else if (mret) begin
                mstatus[3] <= mstatus[7]; mstatus[7] <= 1;
            end
        end
    end
endmodule

// Source: rtl/rv32_core.sv
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
    logic q_valid, q_killed;
    logic [31:0] q_pc, q_pred, q_pred_second;
    logic q_has_second;
    logic id_valid, id_error;
    logic [31:0] id_pc, id_insn, id_pred;
    decode_t id_dec, id_dec_second, x_dec, x_dec_second;
    logic id_second_valid, id_second_error;
    logic [31:0] id_second_insn, id_second_pred;
    logic [2:0] fetch_count, fetch_remaining;
    logic [1:0] fetch_head,fetch_tail;
    wire [1:0] fetch_next_head=fetch_head+2'd1;
    wire [1:0] fetch_next_tail=fetch_tail+2'd1;
    logic [1:0] fetch_pop;
    logic [31:0] fetch_pc [0:3], fetch_insn [0:3], fetch_pred [0:3];
    logic fetch_error [0:3];
    // Small predecode travels with the FIFO. The issue/compaction decision
    // must not wait for the full instruction decoder's result multiplexer.
    logic [6:0] fetch_meta [0:3];
    decode_t response_dec, response_dec_second;
    function automatic [6:0] issue_metadata(input logic [86:0] bits_in);
        decode_t d;
        begin
        d = bits_in;
        issue_metadata = {
            !d.illegal && !d.serial && d.reg_write && d.alu <= ALU_COPY &&
            !(d.load || d.store || d.mul || d.div || d.branch || d.jal || d.jalr),
            d.illegal, d.serial, (d.branch || d.jal || d.jalr),
            d.reg_write, d.use_rs1, d.use_rs2};
        end
    endfunction
    wire [6:0] first_meta = fetch_meta[fetch_head], second_meta = fetch_meta[fetch_next_head];
    wire pair_dependency = id_insn[11:7] != 0 &&
        ((second_meta[1] && id_second_insn[19:15] == id_insn[11:7]) ||
         (second_meta[0] && id_second_insn[24:20] == id_insn[11:7]) ||
         (second_meta[2] && id_second_insn[11:7] == id_insn[11:7]));
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
    logic m_mul;
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
    rv32_decode predecoder (.insn(i_rsp_data[31:0]), .decoded(response_dec));
    rv32_decode predecoder_second (.insn(i_rsp_data[63:32]), .decoded(response_dec_second));
    rv32_alu alu_full (.op(x_dec.alu), .a(alu_a), .b(alu_b), .result(alu_result));
    rv32_alu #(.EXTENSIONS(0)) alu_aux (
        .op(x_dec_second.alu), .a(alu_a_second), .b(alu_b_second), .result(alu_result_second));
    rv32_predictor #(.INDEX_BITS(BTB_BITS)) predictor (
        .clk(clk), .rst(rst), .clear(x_go && x_dec.fence_i),
        .pc(f_pc), .next_pc(prediction),
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
        if (m_valid && m_write_rd && !m_mul && (!m_load || load_bypass_ok)) begin
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
                 ((m_load && !(load_bypass_ok && m_finish && !mem_trap)) || m_mul) &&
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
        id_pred == id_pc + 32'd4 && id_second_pred == id_pc + 32'd8 &&
        !(first_meta[2] && pair_dependency);
    // Dynamically route a simple older ALU to the auxiliary lane, leaving the
    // full lane available to a younger load/store, multiply, divide or branch.
    // Retire in program order and retain the older ALU if the younger faults.
    assign pair_swap_candidate = DUAL_ISSUE && id_second_valid &&
        !id_error && !id_second_error && first_meta[6] &&
        !second_meta[5] && !second_meta[4] &&
        id_pred == id_pc + 32'd4 &&
        !pair_dependency;
    assign pair_swapped = pair_swap_candidate && !pair_normal;
    // Admission does not need the lane-priority decision in its timing path.
    assign pair_eligible = pair_normal || pair_swap_candidate;
    assign pair_issue = id_issue && pair_eligible;
    assign id_valid = fetch_count != 0;
    assign id_pc = fetch_pc[fetch_head];
    assign id_insn = fetch_insn[fetch_head];
    assign id_pred = fetch_pred[fetch_head];
    assign id_error = fetch_error[fetch_head];
    assign id_second_valid = fetch_count >= 2 && fetch_pc[fetch_next_head] == id_pc + 32'd4;
    assign id_second_insn = fetch_insn[fetch_next_head];
    assign id_second_pred = fetch_pred[fetch_next_head];
    assign id_second_error = fetch_error[fetch_next_head];
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
        if (m_valid && m_write_rd && !m_mul && (!m_load || load_bypass_ok)) begin
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
            fetch_count <= 0;fetch_head<=0;fetch_tail<=0;
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
            m_csr_write <= 0; m_mret <= 0; m_serial <= 0; m_mul <= 0;
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
            if (m_ready) begin mul_high_ready <= 0; mul_partial_ready <= 0; end
            else if (m_valid && m_mul && m_funct3 != 0 && !mul_high_ready) begin
                if (MULH_PIPELINE && mul_partial_ready) begin
                    // Optional sum register isolates the MULH carry chain from
                    // result selection and WB. Low-half MUL latency is unchanged.
                    mul_high_result <= product_high; mul_high_ready <= 1;
                end else begin
                    partial_ll_high <= product_ll[31:16];
                    partial_lh <= product_lh; partial_hl <= product_hl; partial_hh <= product_hh;
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
                if (PREDICT_ENABLE && prediction != f_pc + 32'd4) f_pc <= prediction;
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
                q_has_second <= DUAL_ISSUE && (!PREDICT_ENABLE || prediction == f_pc + 32'd4);
            end
            // Four-entry ring: pair admission changes only the read pointer,
            // never a wide payload write mux. Enqueue addresses depend only on
            // the registered tail, independently of this cycle's issue decision.
            fetch_count <= fetch_remaining;
            fetch_head<=fetch_head+fetch_pop;
            if(i_consume && !q_killed)
                fetch_tail<=fetch_tail+(q_has_second?2'd2:2'd1);
            for (fi = 0; fi < 4; fi = fi + 1) begin
                // A flush invalidates count, so the data registers may safely
                // accept stale payloads. Avoid a late branch-to-array mux path.
                if (i_consume && !q_killed) begin
                    if (fetch_tail == fi) begin
                        fetch_pc[fi] <= q_pc; fetch_insn[fi] <= i_rsp_data[31:0];
                        fetch_pred[fi] <= q_pred; fetch_error[fi] <= i_rsp_error[0];
                        fetch_meta[fi] <= issue_metadata(response_dec);
                    end
                    if (q_has_second && fetch_next_tail == fi) begin
                        fetch_pc[fi] <= q_pc + 32'd4; fetch_insn[fi] <= i_rsp_data[63:32];
                        fetch_pred[fi] <= q_pred_second; fetch_error[fi] <= i_rsp_error[1];
                        fetch_meta[fi] <= issue_metadata(response_dec_second);
                    end
                end
            end
            if (i_consume && !q_killed && !front_flush)
                fetch_count <= fetch_remaining + (q_has_second ? 3'd2 : 3'd1);
            if (front_flush) begin fetch_count <= 0;fetch_head<=0;fetch_tail<=0;end
            if (x_ready) begin
                x_valid <= id_issue && !redirect;
                x_second_valid <= pair_issue && !redirect;
                x_swapped <= pair_issue && pair_swapped;
                if (id_issue) begin
                    x_second_pc <= pair_swapped ? id_pc : fetch_pc[fetch_next_head];
                    if (pair_swapped) begin
                        x_dec <= id_dec_second; x_pc <= fetch_pc[fetch_next_head]; x_insn <= id_second_insn;
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
                    m_addr <= memory_address; m_result <= x_result; m_store <= b;
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

// Source: rtl/uart_tx.sv
module uart_tx #(
    parameter integer CLOCK_HZ = 50000000,
    parameter integer BAUD = 115200
) (
    input logic clk, rst, valid,
    input logic [7:0] data,
    output logic ready, tx
);
    localparam integer DIVISOR = CLOCK_HZ / BAUD;
    localparam integer CW = $clog2(DIVISOR + 1);
    localparam logic [31:0] RELOAD = DIVISOR - 1;
    logic [CW-1:0] counter;
    logic [9:0] shift;
    logic [3:0] bits_left;
    assign ready = bits_left == 0;
    assign tx = ready ? 1'b1 : shift[0];
    always_ff @(posedge clk) begin
        if (rst) begin counter <= 0; shift <= '1; bits_left <= 0; end
        else if (valid && ready) begin
            shift <= {1'b1, data, 1'b0};
            bits_left <= 10; counter <= RELOAD[CW-1:0];
        end else if (!ready) begin
            if (counter == 0) begin
                shift <= {1'b1, shift[9:1]};
                bits_left <= bits_left - 4'd1;
                counter <= RELOAD[CW-1:0];
            end else counter <= counter - 1'b1;
        end
    end
endmodule

// Source: rtl/qinglong_extmem.sv
// CPU-side adapter for the 1 GiB DDR window. One 32-byte transaction at a
// time. Optional instruction stream buffer; backing I/D caches sit downstream.
// Instruction fetches crossing a line use two transactions.
module qinglong_extmem #(parameter bit I_BUFFER=0) (
    input logic clk, rst, input logic invalidate_i,
    output logic [31:0] i_buffer_hits,
    input logic i_req_valid, output logic i_req_ready,
    input logic [31:0] i_req_addr,
    output logic i_rsp_valid, input logic i_rsp_ready,
    output logic [63:0] i_rsp_data, output logic [1:0] i_rsp_error,
    input logic d_req_valid, output logic d_req_ready,
    input logic [31:0] d_req_addr, d_req_wdata,
    input logic [3:0] d_req_wstrb, input logic d_req_write,
    output logic d_rsp_valid, input logic d_rsp_ready,
    output logic [31:0] d_rsp_data, output logic d_rsp_error,
    output logic req_valid, input logic req_ready,
    output logic [29:0] req_addr,
    output logic req_write, output logic req_instruction, output logic [255:0] req_wdata,
    output logic [31:0] req_wstrb,
    input logic rsp_valid, output logic rsp_ready,
    input logic [255:0] rsp_data, input logic rsp_error
);
    localparam logic [1:0] IDLE=0, SEND=1, WAIT_RSP=2;
    logic [1:0] state;
    logic prefer_i, owner_i, second_line, last_word;
    logic [2:0] lane;
    logic choose_i;
    logic ib_valid;
    logic [29:0] ib_addr;
    logic [255:0] ib_data;
    wire ib_hit=I_BUFFER && ib_valid && !invalidate_i && i_req_addr[31:30]==2'b10 &&
        i_req_addr[1:0]==0 && i_req_addr[4:2]!=7 && i_req_addr[29:5]==ib_addr[29:5];
    wire [255:0] shifted = rsp_data >> {lane, 5'b0};
    // Registered round-robin grant: neither ready depends on the other
    // request's address/valid. An idle unrequested turn costs at most one cycle.
    wire eligible_i=i_req_valid && (!i_rsp_valid || (I_BUFFER && i_rsp_ready));
    wire eligible_d=d_req_valid && !d_rsp_valid;
    assign choose_i = eligible_i && (!eligible_d || prefer_i);
    assign i_req_ready = !rst && state == IDLE && (!i_rsp_valid || (I_BUFFER && i_rsp_ready)) && choose_i;
    assign d_req_ready = !rst && state == IDLE && !d_rsp_valid && !choose_i;
    assign req_instruction = owner_i;
    assign req_valid = !rst && state == SEND;
    assign rsp_ready = !rst && state == WAIT_RSP;
    always_ff @(posedge clk) begin
        if (rst) begin
            ib_valid<=0;ib_addr<=0;ib_data<=0;i_buffer_hits<=0;
            state <= IDLE; prefer_i <= 0; owner_i <= 0; second_line <= 0;
            last_word <= 0; lane <= 0; req_addr <= 0; req_write <= 0;
            req_wdata <= 0; req_wstrb <= 0;
            i_rsp_valid <= 0; i_rsp_data <= 0; i_rsp_error <= 0;
            d_rsp_valid <= 0; d_rsp_data <= 0; d_rsp_error <= 0;
        end else begin
            if(invalidate_i) ib_valid<=0;
            if (i_rsp_ready) i_rsp_valid <= 0;
            if (d_rsp_ready) d_rsp_valid <= 0;
            if (state == IDLE && !i_rsp_valid && !d_rsp_valid)
                prefer_i <= !prefer_i;
            case (state)
                IDLE: if ((i_req_valid && i_req_ready) || (d_req_valid && d_req_ready)) begin
                    owner_i <= choose_i; prefer_i <= !choose_i; second_line <= 0;
                    req_write <= !choose_i && d_req_write;
                    req_wdata <= {8{d_req_wdata}};
                    req_wstrb <= choose_i ? 32'b0 : ({28'b0, d_req_wstrb} << {d_req_addr[4:2], 2'b0});
                    req_addr <= choose_i ? {i_req_addr[29:5],5'b0} : {d_req_addr[29:5],5'b0};
                    lane <= choose_i ? i_req_addr[4:2] : d_req_addr[4:2];
                    last_word <= i_req_addr[29:2] == 28'hfffffff;
                    if(choose_i) i_rsp_error <= 0; else d_rsp_error <= 0;
                    if(choose_i && ib_hit) begin
                        i_rsp_valid<=1;i_rsp_error<=0;
                        i_rsp_data<=ib_data >> {i_req_addr[4:2],5'b0};
                        i_buffer_hits<=i_buffer_hits+1'b1;
                    end else if (choose_i && (i_req_addr[31:30] != 2'b10 || i_req_addr[1:0] != 0)) begin
                        i_rsp_valid <= 1; i_rsp_error <= 3; i_rsp_data <= 0;
                    end else if (!choose_i && d_req_addr[31:30] != 2'b10) begin
                        d_rsp_valid <= 1; d_rsp_error <= 1; d_rsp_data <= 0;
                    end else state <= SEND;
                end
                SEND: if (req_ready) state <= WAIT_RSP;
                WAIT_RSP: if (rsp_valid) begin
                    state <= IDLE;
                    if(owner_i && I_BUFFER && !invalidate_i && !rsp_error) begin
                        ib_valid<=1;ib_addr<=req_addr;ib_data<=rsp_data;
                    end
                    if (!owner_i) begin
                        d_rsp_valid <= 1; d_rsp_data <= shifted[31:0]; d_rsp_error <= rsp_error;
                    end else if (second_line) begin
                        i_rsp_valid <= 1; i_rsp_data[63:32] <= rsp_data[31:0];
                        i_rsp_error[1] <= rsp_error;
                    end else begin
                        i_rsp_data <= shifted[63:0];
                        i_rsp_error <= rsp_error ? 2'b11 : {last_word, 1'b0};
                        if (lane == 7 && !last_word && !rsp_error) begin
                            second_line <= 1; req_addr <= req_addr + 30'd32; state <= SEND;
                        end else i_rsp_valid <= 1;
                    end
                end
                default: state <= IDLE;
            endcase
        end
    end
endmodule

// Source: rtl/qinglong_soc.sv
// Dual-port TCM + optional uncached DDR window. Default remains TCM-only.
module qinglong_soc #(
    parameter integer RAM_WORDS = 16384,
    parameter INIT_FILE = "build/firmware.hex",
    parameter integer CLOCK_HZ = 50000000,
    parameter bit SIM_FAST_UART = 0,
    parameter bit PREDICT_ENABLE = 1,
    parameter integer BTB_BITS = 8,
    parameter bit DUAL_ISSUE = 1,
    parameter bit LOAD_BYPASS = 1,
    parameter bit LOAD_WORD_ONLY = 0,
    parameter bit EARLY_LSU = 1,
    parameter bit DDR_ENABLE = 0,
    parameter bit CACHE_ENABLE = DDR_ENABLE,
    parameter integer CACHE_SET_BITS = 7,
    parameter bit PIPELINED_MMIO = DDR_ENABLE,
    parameter bit MULH_PIPELINE = 0
) (
    input logic clk, rst,
    input logic irq_external,
    output logic uart_tx_o,
    output logic [31:0] gpio,
    output logic console_valid,
    output logic [7:0] console_data,
    output logic done,
    output logic [31:0] exit_code,
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
    output logic [31:0] branch_count, mispredict_count, load_stall_count,
    input logic ddr_ready, ddr_fault,
    output logic mem_req_valid, input logic mem_req_ready,
    output logic [29:0] mem_req_addr, output logic mem_req_write,
    output logic [255:0] mem_req_wdata, output logic [31:0] mem_req_wstrb,
    input logic mem_rsp_valid, output logic mem_rsp_ready,
    input logic [255:0] mem_rsp_data, input logic mem_rsp_error
);
    localparam integer AW = $clog2(RAM_WORDS);
    logic [31:0] ram [0:RAM_WORDS-1];
    logic iv, ir, irv, irr;
    logic [1:0] ire;
    logic [31:0] ia;
    logic [63:0] idata;
    logic dv, dr, dw, drv, drr, dre;
    logic [31:0] da, dd, drdata;
    logic [3:0] ds;
    logic [31:0] ram_i_data, ram_i_second, ram_d_data, mmio_data;
    logic d_is_ram, d_is_ddr, i_is_ddr;
    logic ddr_d_pending, ddr_d_write;
    logic [31:0] ddr_d_addr, ddr_d_wdata;
    logic [3:0] ddr_d_strb;
    logic [63:0] mtime, mtimecmp;
    logic msip;
    logic tx_ready, tx_valid;
    logic uart_wait;
    logic mmio_pending;
    logic [31:0] mmio_addr, mmio_wdata;
    logic [3:0] mmio_strb;
    logic mmio_write;
    logic [7:0] uart_data;
    wire i_ext = DDR_ENABLE && ia[31:30] == 2'b10;
    wire d_ext = DDR_ENABLE && da[31:30] == 2'b10;
    wire mi_ready, md_ready, mi_valid, md_valid, md_error;
    wire [1:0] mi_error;
    wire [63:0] mi_data;
    wire [31:0] md_data;
    wire cpu_fence_i,cache_busy,cache_error;
    logic cache_flush;
    wire [31:0] cache_ihit,cache_imiss,cache_dhit,cache_dmiss,cache_wb,cache_ib_hit;
    wire mmio_fire = PIPELINED_MMIO ? mmio_pending :
        (dv && dr && !d_ext && da >= RAM_WORDS * 4);
    wire [31:0] peripheral_addr = PIPELINED_MMIO ? mmio_addr : da;
    wire [31:0] peripheral_data = PIPELINED_MMIO ? mmio_wdata : dd;
    wire [3:0] peripheral_strb = PIPELINED_MMIO ? mmio_strb : ds;
    wire peripheral_write = PIPELINED_MMIO ? mmio_write : dw;
    integer j;
    initial begin
        if (INIT_FILE != "") $readmemh(INIT_FILE, ram);
    end
    assign ir = i_ext ? mi_ready : (!irv || irr);
    // Accept the request before decoding a slow peripheral. A UART store
    // holds its response until TX accepts it; ready has no address/data path.
    // Ready depends only on registered queue/response state, never on the
    // newly generated address. DDR requests are decoupled by a one-entry slot.
    assign dr = (!drv || drr) && !uart_wait && !mmio_pending && !ddr_d_pending;
    assign idata = {ram_i_second, ram_i_data};
    // Capture the response source when accepting a request. The select is
    // settled before RAM data arrives and does not depend on DDR rsp_valid.
    assign drdata = DDR_ENABLE ?
        (({32{d_is_ram}} & ram_d_data) | ({32{d_is_ddr}} & md_data) |
         ({32{!d_is_ram && !d_is_ddr}} & mmio_data)) :
        (d_is_ram ? ram_d_data : mmio_data);
    assign tx_valid = uart_wait;
    generate if (DDR_ENABLE) begin : with_ddr
        wire lv,lr,lwrite,linstr,rv,rr,re;
        wire [29:0] la;
        wire [255:0] ld,rd;
        wire [31:0] ls;
        qinglong_extmem #(.I_BUFFER(CACHE_ENABLE)) extmem (
            .invalidate_i(cache_busy || cache_error),.i_buffer_hits(cache_ib_hit),
            .clk(clk), .rst(rst),
            .i_req_valid(iv && i_ext), .i_req_ready(mi_ready), .i_req_addr(ia),
            .i_rsp_valid(mi_valid), .i_rsp_ready(irr), .i_rsp_data(mi_data), .i_rsp_error(mi_error),
            .d_req_valid(ddr_d_pending), .d_req_ready(md_ready), .d_req_addr(ddr_d_addr),
            .d_req_wdata(ddr_d_wdata), .d_req_wstrb(ddr_d_strb), .d_req_write(ddr_d_write),
            .d_rsp_valid(md_valid), .d_rsp_ready(drr), .d_rsp_data(md_data), .d_rsp_error(md_error),
            .req_valid(lv), .req_ready(lr), .req_addr(la), .req_instruction(linstr),
            .req_write(lwrite), .req_wdata(ld), .req_wstrb(ls),
            .rsp_valid(rv), .rsp_ready(rr), .rsp_data(rd), .rsp_error(re)
        );
        if (CACHE_ENABLE) begin : cached
            qinglong_l1cache #(.SET_BITS(CACHE_SET_BITS)) l1 (
                .clk(clk),.rst(rst),.maintenance_req(cpu_fence_i || cache_flush),
                .maintenance_busy(cache_busy),.maintenance_error(cache_error),
                .req_valid(lv),.req_ready(lr),.req_addr(la),.req_instruction(linstr),
                .req_write(lwrite),.req_wdata(ld),.req_wstrb(ls),
                .rsp_valid(rv),.rsp_ready(rr),.rsp_data(rd),.rsp_error(re),
                .mem_req_valid(mem_req_valid),.mem_req_ready(mem_req_ready),.mem_req_addr(mem_req_addr),
                .mem_req_write(mem_req_write),.mem_req_wdata(mem_req_wdata),.mem_req_wstrb(mem_req_wstrb),
                .mem_rsp_valid(mem_rsp_valid),.mem_rsp_ready(mem_rsp_ready),.mem_rsp_data(mem_rsp_data),.mem_rsp_error(mem_rsp_error),
                .i_hits(cache_ihit),.i_misses(cache_imiss),.d_hits(cache_dhit),.d_misses(cache_dmiss),.writebacks(cache_wb));
        end else begin : uncached
            assign mem_req_valid=lv;assign lr=mem_req_ready;assign mem_req_addr=la;
            assign mem_req_write=lwrite;assign mem_req_wdata=ld;assign mem_req_wstrb=ls;
            assign rv=mem_rsp_valid;assign mem_rsp_ready=rr;assign rd=mem_rsp_data;assign re=mem_rsp_error;
            assign cache_busy=0;assign cache_error=0;assign cache_ihit=0;assign cache_imiss=0;
            assign cache_dhit=0;assign cache_dmiss=0;assign cache_wb=0;
        end
    end else begin : without_ddr
        assign cache_ib_hit=0;
        assign mi_ready = 0; assign md_ready = 0; assign mi_valid = 0; assign md_valid = 0;
        assign mi_error = 0; assign md_error = 0; assign mi_data = 0; assign md_data = 0;
        assign cache_busy=0;assign cache_error=0;assign cache_ihit=0;assign cache_imiss=0;
        assign cache_dhit=0;assign cache_dmiss=0;assign cache_wb=0;
        assign mem_req_valid = 0; assign mem_req_addr = 0; assign mem_req_write = 0;
        assign mem_req_wdata = 0; assign mem_req_wstrb = 0; assign mem_rsp_ready = 0;
    end endgenerate
    uart_tx #(.CLOCK_HZ(CLOCK_HZ)) tx_unit (
        .clk(clk), .rst(rst), .valid(tx_valid), .data(uart_data), .ready(tx_ready), .tx(uart_tx_o)
    );
    rv32_core #(.PREDICT_ENABLE(PREDICT_ENABLE), .BTB_BITS(BTB_BITS),
                .DUAL_ISSUE(DUAL_ISSUE), .LOAD_BYPASS(LOAD_BYPASS), .LOAD_WORD_ONLY(LOAD_WORD_ONLY), .EARLY_LSU(EARLY_LSU),
                .MULH_PIPELINE(MULH_PIPELINE)) cpu (
        .clk(clk), .rst(rst),
        .fence_i_o(cpu_fence_i), .i_req_valid(iv), .i_req_ready(ir), .i_req_addr(ia),
        .i_rsp_valid(irv || mi_valid), .i_rsp_ready(irr),
        .i_rsp_data(i_is_ddr ? mi_data : idata), .i_rsp_error(i_is_ddr ? mi_error : ire),
        .d_req_valid(dv), .d_req_ready(dr), .d_req_addr(da), .d_req_wdata(dd),
        .d_req_wstrb(ds), .d_req_write(dw),
        .d_rsp_valid(drv || md_valid), .d_rsp_ready(drr),
        .d_rsp_data(drdata), .d_rsp_error(d_is_ddr ? md_error : dre),
        .irq_software(msip), .irq_timer(mtime >= mtimecmp), .irq_external(irq_external),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_insn(retire_insn),
        .retire_data(retire_data), .retire_rd(retire_rd),
        .retire_second_valid(retire_second_valid), .retire_second_pc(retire_second_pc),
        .retire_second_insn(retire_second_insn), .retire_second_data(retire_second_data),
        .retire_second_rd(retire_second_rd), .dual_retire_count(dual_retire_count),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause),
        .trap_value(trap_value), .cycle_count(cycle_count), .instret_count(instret_count),
        .branch_count(branch_count), .mispredict_count(mispredict_count),
        .load_stall_count(load_stall_count)
    );
    // No reset on data or arrays: preserve synchronous block-RAM inference.
    // Software must FENCE.I after code writes; concurrent same-address port
    // read/write data is deliberately not relied on by the CPU/software.
    always_ff @(posedge clk) begin
        if (iv && ir && ia < RAM_WORDS * 4)
            ram_i_data <= ram[ia[AW+1:2]];
        if (iv && ir && ia < RAM_WORDS * 4 - 4)
            ram_i_second <= ram[ia[AW+1:2] + 1'b1];
        // Speculate reads while the response slot is free. Address range and
        // request-valid stay on write enables only; held responses stay stable.
        if (!drv || drr) ram_d_data <= ram[da[AW+1:2]];
        if (dv && dr && da < RAM_WORDS * 4) begin
            if (dw)
                for (j = 0; j < 4; j = j + 1)
                    if (ds[j]) ram[da[AW+1:2]][8*j +: 8] <= dd[8*j +: 8];
        end
    end
    always_ff @(posedge clk) begin
        if (rst) begin
            irv <= 0; ire <= 0; drv <= 0; dre <= 0; d_is_ram <= 0; d_is_ddr <= 0; i_is_ddr <= 0;
            mmio_data <= 0; gpio <= 0; done <= 0; exit_code <= 0;
            console_valid <= 0; console_data <= 0; mtime <= 0; mtimecmp <= '1; msip <= 0;
            uart_wait <= 0; uart_data <= 0;
            ddr_d_pending <= 0; ddr_d_write <= 0; ddr_d_addr <= 0; ddr_d_wdata <= 0; ddr_d_strb <= 0;
            cache_flush <= 0;
            mmio_pending <= 0; mmio_addr <= 0; mmio_wdata <= 0; mmio_strb <= 0; mmio_write <= 0;
        end else begin
            cache_flush <= 0;
            mtime <= mtime + 64'd1;
            if (ddr_d_pending && md_ready) ddr_d_pending <= 0;
            if (DDR_ENABLE && dv && dr && d_ext) begin
                ddr_d_pending <= 1; ddr_d_addr <= da;
                ddr_d_wdata <= dd; ddr_d_strb <= ds; ddr_d_write <= dw;
            end
            console_valid <= 0;
            if (iv && ir) i_is_ddr <= i_ext;
            if (dv && dr) begin
                d_is_ram <= da < RAM_WORDS * 4;
                d_is_ddr <= d_ext;
            end
            if (irr) irv <= 0;
            if (iv && ir && !i_ext) begin
                irv <= 1;
                ire <= {ia >= RAM_WORDS * 4 - 4, ia >= RAM_WORDS * 4};
            end
            if (drr) drv <= 0;
            if (uart_wait && tx_ready) begin
                uart_wait <= 0; drv <= 1;
                console_valid <= 1; console_data <= uart_data;
            end
            if (mmio_pending) mmio_pending <= 0;
            if (dv && dr && !d_ext) begin
                drv <= 1; dre <= 0; mmio_data <= 0;
                if (PIPELINED_MMIO && da >= RAM_WORDS * 4) begin
                    // Accept the address before decoding peripheral registers.
                    // The CPU waits for the response; TCM retains its fast path.
                    mmio_pending <= 1; drv <= 0;
                    mmio_addr <= da; mmio_wdata <= dd;
                    mmio_strb <= ds; mmio_write <= dw;
                end
            end
            if (mmio_fire) begin
                drv <= 1; dre <= 0; mmio_data <= 0;
                case (peripheral_addr)
                    32'h10000000: begin
                        mmio_data <= {31'b0, (tx_ready || SIM_FAST_UART)};
                        if (peripheral_write && peripheral_strb[0]) begin
                            if (SIM_FAST_UART) begin
                                console_valid <= 1; console_data <= peripheral_data[7:0];
                            end else begin
                                uart_wait <= 1; uart_data <= peripheral_data[7:0]; drv <= 0;
                            end
                        end
                    end
                    32'h10000004: if (peripheral_write && peripheral_strb == 15) begin done <= 1; exit_code <= peripheral_data; end
                    32'h10000008: begin
                        mmio_data <= gpio;
                        if (peripheral_write) for (j = 0; j < 4; j = j + 1)
                            if (peripheral_strb[j]) gpio[8*j +: 8] <= peripheral_data[8*j +: 8];
                    end
                    32'h10000100: mmio_data <= mtime[31:0];
                    32'h10000104: mmio_data <= mtime[63:32];
                    32'h10000108: begin
                        mmio_data <= mtimecmp[31:0];
                        if (peripheral_write && peripheral_strb == 15) mtimecmp[31:0] <= peripheral_data;
                    end
                    32'h1000010c: begin
                        mmio_data <= mtimecmp[63:32];
                        if (peripheral_write && peripheral_strb == 15) mtimecmp[63:32] <= peripheral_data;
                    end
                    32'h10000110: begin
                        mmio_data <= {31'b0, msip};
                        if (peripheral_write && peripheral_strb[0]) msip <= peripheral_data[0];
                    end
                    32'h10000200: begin
                        // bit 0: calibrated and usable, bit 1: sticky fault,
                        // bit 2: external-memory interface compiled in.
                        if (DDR_ENABLE) begin
                            mmio_data <= {26'b0, CACHE_ENABLE, cache_error, (cache_busy || cache_flush), 1'b1, ddr_fault, ddr_ready};
                            if (peripheral_write) dre <= 1;
                        end else dre <= 1;
                    end
                    32'h10000204: begin
                        if(CACHE_ENABLE && DDR_ENABLE) begin
                            mmio_data <= {30'b0,cache_error,(cache_busy || cache_flush)};
                            if(peripheral_write && peripheral_strb[0] && peripheral_data[0]) cache_flush<=1;
                        end else dre<=1;
                    end
                    32'h10000210: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_ihit;else dre<=1;
                    32'h10000214: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_imiss;else dre<=1;
                    32'h10000218: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_dhit;else dre<=1;
                    32'h1000021c: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_dmiss;else dre<=1;
                    32'h10000220: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_wb;else dre<=1;
                    32'h10000224: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_ib_hit;else dre<=1;
                    default: dre <= 1;
                endcase
            end
        end
    end
endmodule

// Source: rtl/qinglong_l1cache.sv
// Two independent 2-way caches. Synchronous line/tag reads infer block RAM.
// One external 32-byte transaction at a time; D is write-back/write-allocate.
module qinglong_l1cache #(parameter integer SET_BITS=7) (
 input logic clk,rst, maintenance_req,
 output logic maintenance_busy,maintenance_error,
 input logic req_valid, output logic req_ready,
 input logic req_instruction,req_write,
 input logic [29:0] req_addr,
 input logic [255:0] req_wdata,input logic [31:0] req_wstrb,
 output logic rsp_valid,input logic rsp_ready,
 output logic [255:0] rsp_data,output logic rsp_error,
 output logic mem_req_valid,input logic mem_req_ready,
 output logic [29:0] mem_req_addr,output logic mem_req_write,
 output logic [255:0] mem_req_wdata,output logic [31:0] mem_req_wstrb,
 input logic mem_rsp_valid,output logic mem_rsp_ready,
 input logic [255:0] mem_rsp_data,input logic mem_rsp_error,
 output logic [31:0] i_hits,i_misses,d_hits,d_misses,writebacks
);
 localparam integer SETS=1<<SET_BITS, TAG_BITS=25-SET_BITS;
 localparam [3:0] IDLE=0,LOOKUP=1,WB_SEND=2,WB_WAIT=3,FILL_SEND=4,FILL_WAIT=5,SCAN_READ=6,SCAN_CHECK=7,RESET_SCAN=8;
 logic [3:0] state;
 logic [255:0] i_data0[0:SETS-1],i_data1[0:SETS-1],d_data0[0:SETS-1],d_data1[0:SETS-1];
 logic [TAG_BITS-1:0] i_tag0[0:SETS-1],i_tag1[0:SETS-1],d_tag0[0:SETS-1],d_tag1[0:SETS-1];
 // Small synchronous metadata RAMs replace resettable bitmaps and their
 // variable-index write decoders. Reset walks the sets instead of resetting
 // every bit, allowing RAM inference. Layout: I={LRU,V1,V0}, D={LRU,D1,D0,V1,V0}.
 logic [2:0] i_meta[0:SETS-1],imr;
 logic [4:0] d_meta[0:SETS-1],dmr;
 logic [255:0] ir0,ir1,dr0,dr1,evict_data;
 logic [TAG_BITS-1:0] it0,it1,dt0,dt1;
 logic instruction,writing,victim,flushing,flush_pending;
 wire [255:0] rd0=(instruction && !flushing)?ir0:dr0, rd1=(instruction && !flushing)?ir1:dr1;
 wire [TAG_BITS-1:0] rt0=(instruction && !flushing)?it0:dt0, rt1=(instruction && !flushing)?it1:dt1;
 logic [29:0] address,evict_addr;
 logic [255:0] write_data;
 logic [31:0] write_mask;
 logic [SET_BITS-1:0] scan;
 wire [SET_BITS-1:0] index=address[5+:SET_BITS];
 wire [TAG_BITS-1:0] tag=address[29:5+SET_BITS];
 wire valid0=(instruction && !flushing)?imr[0]:dmr[0];
 wire valid1=(instruction && !flushing)?imr[1]:dmr[1];
 wire read_lru=instruction?imr[2]:dmr[4];
 wire hit0=valid0 && rt0==tag, hit1=valid1 && rt1==tag;
 wire chosen=hit0?1'b0:hit1?1'b1:!valid0?1'b0:!valid1?1'b1:read_lru;
 function automatic [255:0] merge_bytes(input logic [255:0] old_data,new_data,input logic [31:0] mask);
  integer b;
  begin
   for(b=0;b<32;b=b+1) merge_bytes[8*b+:8]=mask[b]?new_data[8*b+:8]:old_data[8*b+:8];
  end
 endfunction
 assign maintenance_busy=flush_pending || flushing || state==RESET_SCAN;
 assign req_ready=!rst && state==IDLE && !rsp_valid && !flush_pending;
 assign mem_req_valid=!rst && (state==WB_SEND || state==FILL_SEND);
 assign mem_req_write=state==WB_SEND;
 assign mem_req_addr=state==WB_SEND?evict_addr:address;
 assign mem_req_wdata=evict_data;
 assign mem_req_wstrb=state==WB_SEND?32'hffffffff:32'b0;
 assign mem_rsp_ready=!rst && (state==WB_WAIT || state==FILL_WAIT);
 wire accept=req_valid && req_ready;
 wire d_read=(accept && !req_instruction) || state==SCAN_READ;
 wire [SET_BITS-1:0] d_read_index=state==SCAN_READ?scan:req_addr[5+:SET_BITS];
 // A single read address per D array prevents PDS from replicating RAMs for
 // demand lookup and maintenance. No reset of any line/tag/metadata array.
 always_ff @(posedge clk) begin
  if(accept && req_instruction) begin
    imr<=i_meta[req_addr[5+:SET_BITS]];
    ir0<=i_data0[req_addr[5+:SET_BITS]];ir1<=i_data1[req_addr[5+:SET_BITS]];
    it0<=i_tag0[req_addr[5+:SET_BITS]];it1<=i_tag1[req_addr[5+:SET_BITS]];
  end
  if(d_read) begin
   dmr<=d_meta[d_read_index];
   dr0<=d_data0[d_read_index];dr1<=d_data1[d_read_index];
   dt0<=d_tag0[d_read_index];dt1<=d_tag1[d_read_index];
  end else if(state==WB_WAIT && mem_rsp_valid && !mem_rsp_error) begin
   // Recheck the other way during maintenance without rereading the data.
   if(victim) dmr[3]<=0;else dmr[2]<=0;
  end
 end
 always_ff @(posedge clk) begin
  if(rst) begin
   state<=RESET_SCAN;
   flush_pending<=0;flushing<=0;maintenance_error<=0;scan<=0;
   rsp_valid<=0;rsp_data<=0;rsp_error<=0;
   address<=0;instruction<=0;writing<=0;write_data<=0;write_mask<=0;
   evict_addr<=0;evict_data<=0;victim<=0;
   i_hits<=0;i_misses<=0;d_hits<=0;d_misses<=0;writebacks<=0;
  end else begin
   if(rsp_ready) rsp_valid<=0;
   if(maintenance_req) flush_pending<=1;
   case(state)
    RESET_SCAN: begin
     i_meta[scan]<=0;d_meta[scan]<=0;
     if(&scan) begin scan<=0;state<=IDLE;end
     else scan<=scan+1'b1;
    end
    IDLE: begin
     if(flush_pending && !rsp_valid) begin
      flushing<=1;flush_pending<=0;scan<=0;state<=SCAN_READ;
     end else if(req_valid && req_ready) begin
      address<=req_addr;instruction<=req_instruction;writing<=req_write;
      write_data<=req_wdata;write_mask<=req_wstrb;
      rsp_error<=0;
      if(maintenance_error || req_addr[4:0]!=0 || (req_instruction && req_write)) begin
       rsp_valid<=1;rsp_error<=1;rsp_data<=0;
      end else state<=LOOKUP;
     end
    end
    LOOKUP: begin
     if(hit0 || hit1) begin
      if(instruction) begin
       i_hits<=i_hits+1'b1;i_meta[index]<={!chosen,imr[1:0]};
      end else begin
       d_hits<=d_hits+1'b1;
       d_meta[index]<={!chosen,dmr[3] || (writing && chosen),dmr[2] || (writing && !chosen),dmr[1:0]};
       if(writing) begin
        if(chosen) d_data1[index]<=merge_bytes(rd1,write_data,write_mask);
        else d_data0[index]<=merge_bytes(rd0,write_data,write_mask);
       end
      end
      rsp_data<=writing?merge_bytes(chosen?rd1:rd0,write_data,write_mask):(chosen?rd1:rd0);
      rsp_valid<=1;rsp_error<=0;state<=IDLE;
     end else begin
      if(instruction) i_misses<=i_misses+1'b1;else d_misses<=d_misses+1'b1;
      victim<=chosen;evict_addr<={chosen?rt1:rt0,index,5'b0};evict_data<=chosen?rd1:rd0;
      if(!instruction && (chosen?(valid1&&dmr[3]):(valid0&&dmr[2]))) state<=WB_SEND;
      else state<=FILL_SEND;
     end
    end
    WB_SEND: if(mem_req_ready) state<=WB_WAIT;
    WB_WAIT: if(mem_rsp_valid) begin
     if(mem_rsp_error) begin
      if(flushing) begin maintenance_error<=1;flushing<=0;end
      else begin rsp_error<=1;rsp_data<=0;rsp_valid<=1;end
      // Preserve dirty victim on failed writeback, never install the new line.
      state<=IDLE;
     end else begin
      writebacks<=writebacks+1'b1;
      if(flushing) begin
       d_meta[scan]<=victim?{dmr[4],1'b0,dmr[2:0]}:{dmr[4:3],1'b0,dmr[1:0]};
       state<=SCAN_CHECK;
      end else begin
       d_meta[index]<=victim?{dmr[4],1'b0,dmr[2:0]}:{dmr[4:3],1'b0,dmr[1:0]};
       state<=FILL_SEND;
      end
     end
    end
    FILL_SEND: if(mem_req_ready) state<=FILL_WAIT;
    FILL_WAIT: if(mem_rsp_valid) begin
     rsp_valid<=1;rsp_error<=mem_rsp_error;
     rsp_data<=mem_rsp_error?256'b0:(writing?merge_bytes(mem_rsp_data,write_data,write_mask):mem_rsp_data);
     if(!mem_rsp_error) begin
      if(instruction) begin
       i_meta[index]<={!victim,victim?1'b1:valid1,victim?valid0:1'b1};
       if(victim) begin i_data1[index]<=mem_rsp_data;i_tag1[index]<=tag;end
       else begin i_data0[index]<=mem_rsp_data;i_tag0[index]<=tag;end
      end else begin
       d_meta[index]<={!victim,victim?writing:dmr[3],victim?dmr[2]:writing,victim?1'b1:valid1,victim?valid0:1'b1};
       if(victim) begin d_data1[index]<=writing?merge_bytes(mem_rsp_data,write_data,write_mask):mem_rsp_data;d_tag1[index]<=tag;end
       else begin d_data0[index]<=writing?merge_bytes(mem_rsp_data,write_data,write_mask):mem_rsp_data;d_tag0[index]<=tag;end
      end
     end
     state<=IDLE;
    end
    SCAN_READ: state<=SCAN_CHECK;
    SCAN_CHECK: begin
     if((dmr[0]&&dmr[2]) || (dmr[1]&&dmr[3])) begin
      victim<=!(dmr[0]&&dmr[2]);
      evict_addr<={(dmr[0]&&dmr[2])?rt0:rt1,scan,5'b0};
      evict_data<=(dmr[0]&&dmr[2])?rd0:rd1;
      state<=WB_SEND;
     end else begin
      i_meta[scan]<=0;d_meta[scan]<=0;
      if(&scan) begin flushing<=0;state<=IDLE;end
      else begin scan<=scan+1'b1;state<=SCAN_READ;end
     end
    end
    default:state<=IDLE;
   endcase
  end
 end
endmodule

// Source: rtl/qinglong_hmic_bridge.sv
// Bundled-data mailbox + Logos2 HMIC_S simplified AXI adapter (UG042003).
// Both domains MUST share reset_n; payload registers remain stable throughout
// the toggle handshake. Constrain their CDC flight time, not just the toggles.
// HMIC has no WVALID, B channel or RREADY. Reserve storage before AR; retain
// WDATA/WSTRB before AW and through WREADY. Completion means controller acceptance.
module qinglong_hmic_bridge #(
    parameter integer TIMEOUT_BITS = 20
) (
    input logic cpu_clk, ui_clk, reset_n,
    input logic init_done,
    output logic ready_cpu, fault_cpu,
    input logic req_valid, output logic req_ready,
    input logic [29:0] req_addr, input logic req_write,
    input logic [255:0] req_wdata, input logic [31:0] req_wstrb,
    output logic rsp_valid, input logic rsp_ready,
    output logic [255:0] rsp_data, output logic rsp_error,
    output logic [27:0] axi_awaddr, axi_araddr,
    output logic axi_awvalid, axi_arvalid,
    input logic axi_awready, axi_arready,
    output logic [255:0] axi_wdata, output logic [31:0] axi_wstrb,
    input logic axi_wready, axi_wusero_last, input logic [3:0] axi_wusero_id,
    input logic [255:0] axi_rdata, input logic axi_rvalid, axi_rlast,
    input logic [3:0] axi_rid
);
    (* async_reg = "true" *) logic [2:0] cpu_reset, ui_reset;
    (* async_reg = "true" *) logic [1:0] req_sync, ack_sync, init_sync, fault_sync;
    logic req_toggle, ack_toggle, busy, local_fault, ui_fault, ever_ready;
    logic [TIMEOUT_BITS-1:0] cpu_timer, ui_timer;
    // Source-domain holding registers, sampled after the associated toggle.
    logic [29:0] hold_addr;
    logic hold_write;
    logic [255:0] hold_wdata, return_data;
    logic [31:0] hold_wstrb;
    logic return_error;
    localparam logic [1:0] IDLE=0, AW=1, W=2, AR_R=3;
    logic [1:0] state;
    logic ar_sent;
    always_ff @(posedge cpu_clk or negedge reset_n)
        if (!reset_n) cpu_reset <= 3'b111;
        else cpu_reset <= {cpu_reset[1:0], 1'b0};
    always_ff @(posedge ui_clk or negedge reset_n)
        if (!reset_n) ui_reset <= 3'b111;
        else ui_reset <= {ui_reset[1:0], 1'b0};
    assign ready_cpu = init_sync[1] && !fault_cpu && !cpu_reset[2];
    assign fault_cpu = local_fault || fault_sync[1];
    assign req_ready = !cpu_reset[2] && !busy && !rsp_valid;
    assign axi_awvalid = state == AW && init_done && !ui_fault && !ui_reset[2];
    assign axi_arvalid = state == AR_R && !ar_sent && init_done && !ui_fault && !ui_reset[2];
    assign axi_araddr = axi_awaddr;
    always_ff @(posedge cpu_clk or negedge reset_n) begin
        if (!reset_n) begin
            ack_sync <= 0; init_sync <= 0; fault_sync <= 0;
        end else begin
            ack_sync <= {ack_sync[0], ack_toggle};
            init_sync <= {init_sync[0], init_done};
            fault_sync <= {fault_sync[0], ui_fault};
        end
    end
    always_ff @(posedge cpu_clk or negedge reset_n) begin
        if (!reset_n) begin
            req_toggle <= 0; busy <= 0; local_fault <= 0; cpu_timer <= 0;
            hold_addr <= 0; hold_write <= 0; hold_wdata <= 0; hold_wstrb <= 0;
            rsp_valid <= 0; rsp_data <= 0; rsp_error <= 0;
        end else if (!cpu_reset[2]) begin
            if (rsp_ready) rsp_valid <= 0;
            if (req_valid && req_ready) begin
                cpu_timer <= 0;
                if (!ready_cpu || req_addr[4:0] != 0) begin
                    rsp_valid <= 1; rsp_data <= 0; rsp_error <= 1;
                end else begin
                    hold_addr <= req_addr; hold_write <= req_write;
                    hold_wdata <= req_wdata; hold_wstrb <= req_wstrb;
                    req_toggle <= !req_toggle; busy <= 1;
                end
            end
            if (busy) begin
                cpu_timer <= cpu_timer + 1'b1;
                if (ack_sync[1] == req_toggle) begin
                    busy <= 0; rsp_valid <= 1; rsp_data <= return_data; rsp_error <= return_error;
                end else if (&cpu_timer) begin
                    // Never reuse a timed-out mailbox: a late DDR response may
                    // still arrive. Only a common controller/bridge reset recovers.
                    busy <= 0; local_fault <= 1; rsp_valid <= 1; rsp_error <= 1; rsp_data <= 0;
                end
            end
        end
    end
    always_ff @(posedge ui_clk or negedge reset_n)
        if (!reset_n) req_sync <= 0;
        else req_sync <= {req_sync[0], req_toggle};

    always_ff @(posedge ui_clk or negedge reset_n) begin
        if (!reset_n) begin
            state <= IDLE; ack_toggle <= 0; return_data <= 0; return_error <= 0;
            ui_fault <= 0; ever_ready <= 0; ui_timer <= 0; ar_sent <= 0;
            axi_awaddr <= 0; axi_wdata <= 0; axi_wstrb <= 0;
        end else if (!ui_reset[2]) begin
            if (init_done) ever_ready <= 1;
            if (ever_ready && !init_done) ui_fault <= 1;
            if (state == IDLE) begin
                ui_timer <= 0; ar_sent <= 0;
                if (req_sync[1] != ack_toggle) begin
                    if (!init_done || ui_fault) begin
                        return_error <= 1; return_data <= 0; ack_toggle <= req_sync[1];
                    end else begin
                        axi_awaddr <= hold_addr[29:2];
                        axi_wdata <= hold_wdata; axi_wstrb <= hold_wstrb;
                        state <= hold_write ? AW : AR_R;
                    end
                end
            end else begin
                ui_timer <= ui_timer + 1'b1;
                if (!init_done || ui_fault || &ui_timer) begin
                    ui_fault <= 1; return_error <= 1; return_data <= 0;
                    ack_toggle <= req_sync[1]; state <= IDLE;
                end else if ((state == AW && axi_awready && axi_wready) || (state == W && axi_wready)) begin
                    return_error <= !axi_wusero_last || axi_wusero_id != 0;
                    ui_fault <= !axi_wusero_last || axi_wusero_id != 0;
                    return_data <= 0; ack_toggle <= req_sync[1]; state <= IDLE;
                end else if (state == AW && axi_awready) state <= W;
                else if (state == AR_R) begin
                    if (axi_arready) ar_sent <= 1;
                    if (axi_rvalid && (ar_sent || axi_arready)) begin
                        return_data <= axi_rdata;
                        return_error <= !axi_rlast || axi_rid != 0;
                        ui_fault <= !axi_rlast || axi_rid != 0;
                        ack_toggle <= req_sync[1]; state <= IDLE;
                    end
                end
            end
        end
    end
endmodule

// Source: fpga/board/qinglong_board_clock.sv
// Logos2 GPLL configuration; primitive declaration/model is supplied by PDS.
// UG041001: VCO = Fin * MDIV * FDIV / IDIV; Fout = VCO / ODIV.
// 27 -> 108 MHz: 27 * 4 * 8 / 1 = 864 MHz VCO, divided by 8.
// 27 -> 81 MHz: 27 * 3 * 8 / 1 = 648 MHz VCO, divided by 8.
// 27 -> 96 MHz: 27 * 16 * 2 / 1 = 864 MHz VCO, divided by 9.
// 27 -> 102 MHz: 27 * 17 * 2 / 1 = 918 MHz VCO, divided by 9.
module qinglong_board_clock #(
    parameter integer CLOCK_MHZ = 108
) (
    input wire clk_27m,
    output wire cpu_clk, locked
);
    generate if (CLOCK_MHZ == 27) begin : bypass
        assign cpu_clk = clk_27m;
        assign locked = 1'b1;
    end else begin : pll_mode
        wire pll_clk;
        GTP_GPLL #(
            .CLKIN_FREQ(27.0), .STATIC_RATIOI(1),
            .STATIC_RATIOM((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? CLOCK_MHZ / 6 : CLOCK_MHZ / 27),
            .STATIC_RATIOF((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 2.0 : 8.0),
            .STATIC_RATIO0((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 9.0 : 8.0),
            .STATIC_DUTYF((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 2 : 8),
            .STATIC_DUTY0((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 9 : 8),
            .INTERNAL_FB("CLKOUTF"), .EXTERNAL_FB("DISABLE"),
            .LOCK_MODE(1'b0)
        ) pll (
            .CLKIN1(clk_27m), .CLKIN2(1'b0), .CLKIN_SEL(1'b0),
            // Keep the PLL running across CPU reset-button presses. Its
            // configuration reset is provided by FPGA configuration itself.
            .CLKFB(1'b0), .RST(1'b0), .PLL_PWD(1'b0),
            .CLKOUT0(pll_clk), .LOCK(locked),
            .DPS_CLK(1'b0), .DPS_EN(1'b0), .DPS_DIR(1'b0),
            .CLKOUT0_SYN(1'b0), .CLKOUT1_SYN(1'b0),
            .CLKOUT2_SYN(1'b0), .CLKOUT3_SYN(1'b0),
            .CLKOUT4_SYN(1'b0), .CLKOUT5_SYN(1'b0),
            .CLKOUT6_SYN(1'b0), .CLKOUTF_SYN(1'b0),
            .APB_CLK(1'b0), .APB_RST_N(1'b0), .APB_ADDR(5'b0),
            .APB_SEL(1'b0), .APB_EN(1'b0), .APB_WRITE(1'b0), .APB_WDATA(16'b0)
        );
        GTP_CLKBUFG cpu_clock_buffer (.CLKIN(pll_clk), .CLKOUT(cpu_clk));
    end endgenerate
endmodule

// Source: fpga/board/qinglong_ddr_board_top.sv
// MES2L676-100HP: CPU 102 MHz, DDR3 1000 MT/s, HMIC UI 125 MHz.
// This is a separate integration profile; the TCM CoreMark build is unchanged.
module qinglong_board_top #(
    parameter integer CLOCK_MHZ = 102,
    parameter bit BOARD_MULH_PIPELINE = 1,
    parameter bit BOARD_CACHE_ENABLE = 1,
    parameter bit BOARD_LOAD_WORD_ONLY = 0,
    parameter integer BOARD_CACHE_SET_BITS = 8
) (
    input wire clk_27m, reset_n, ref_clk_p, ref_clk_n,
    output wire uart_txd, output wire [7:0] led,
    output wire mem_cs_n, mem_rst_n, mem_ck, mem_ck_n, mem_cke,
    output wire mem_ras_n, mem_cas_n, mem_we_n, mem_odt,
    output wire [14:0] mem_a, output wire [2:0] mem_ba,
    inout wire [3:0] mem_dqs, mem_dqs_n,
    inout wire [31:0] mem_dq, output wire [3:0] mem_dm
);
    wire cpu_clk, cpu_locked, ui_clk, ref_clk, init_done;
    wire common_reset_n = reset_n && cpu_locked;
    logic [2:0] reset_sync = 3'b111;
    wire ddr_ready, ddr_fault, done;
    wire [31:0] gpio, exit_code;
    wire req_valid, req_ready, req_write, rsp_valid, rsp_ready, rsp_error;
    wire [29:0] req_addr;
    wire [255:0] req_wdata, rsp_data;
    wire [31:0] req_wstrb;
    wire [27:0] axi_awaddr, axi_araddr;
    wire axi_awvalid, axi_awready, axi_arvalid, axi_arready;
    wire [255:0] axi_wdata, axi_rdata;
    wire [31:0] axi_wstrb;
    wire axi_wready, axi_wusero_last, axi_rvalid, axi_rlast;
    wire [3:0] axi_wusero_id, axi_rid;
    qinglong_board_clock #(.CLOCK_MHZ(CLOCK_MHZ)) clocking_unit (
        .clk_27m(clk_27m), .cpu_clk(cpu_clk), .locked(cpu_locked));
    GTP_INBUFDS refclk_inbuf (.I(ref_clk_p), .IB(ref_clk_n), .O(ref_clk));
    always_ff @(posedge cpu_clk or negedge common_reset_n)
        if (!common_reset_n) reset_sync <= 3'b111;
        else reset_sync <= {reset_sync[1:0], 1'b0};
    assign led = {ddr_fault, done && exit_code != 0, 3'b0, ddr_ready, done && exit_code == 0, gpio[0]};
    qinglong_soc #(.CLOCK_HZ(CLOCK_MHZ*1000000), .INIT_FILE("firmware.hex"), .DDR_ENABLE(1),
                  .MULH_PIPELINE(BOARD_MULH_PIPELINE), .CACHE_ENABLE(BOARD_CACHE_ENABLE),
                  .CACHE_SET_BITS(BOARD_CACHE_SET_BITS), .LOAD_WORD_ONLY(BOARD_LOAD_WORD_ONLY)) soc (
        .clk(cpu_clk), .rst(reset_sync[2]), .irq_external(1'b0),
        .uart_tx_o(uart_txd), .gpio(gpio), .done(done), .exit_code(exit_code),
        .ddr_ready(ddr_ready), .ddr_fault(ddr_fault),
        .mem_req_valid(req_valid), .mem_req_ready(req_ready), .mem_req_addr(req_addr),
        .mem_req_write(req_write), .mem_req_wdata(req_wdata), .mem_req_wstrb(req_wstrb),
        .mem_rsp_valid(rsp_valid), .mem_rsp_ready(rsp_ready), .mem_rsp_data(rsp_data), .mem_rsp_error(rsp_error)
    );
    qinglong_hmic_bridge bridge (
        .cpu_clk(cpu_clk), .ui_clk(ui_clk), .reset_n(common_reset_n), .init_done(init_done),
        .ready_cpu(ddr_ready), .fault_cpu(ddr_fault),
        .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr), .req_write(req_write),
        .req_wdata(req_wdata), .req_wstrb(req_wstrb),
        .rsp_valid(rsp_valid), .rsp_ready(rsp_ready), .rsp_data(rsp_data), .rsp_error(rsp_error),
        .axi_awaddr(axi_awaddr), .axi_awvalid(axi_awvalid), .axi_awready(axi_awready),
        .axi_araddr(axi_araddr), .axi_arvalid(axi_arvalid), .axi_arready(axi_arready),
        .axi_wdata(axi_wdata), .axi_wstrb(axi_wstrb), .axi_wready(axi_wready),
        .axi_wusero_last(axi_wusero_last), .axi_wusero_id(axi_wusero_id),
        .axi_rdata(axi_rdata), .axi_rvalid(axi_rvalid), .axi_rlast(axi_rlast), .axi_rid(axi_rid)
    );
    // Preserve vendor instance name so the generated PHY constraints apply.
    qinglong_ddr3 I_ips_ddr_top (
        .ref_clk(ref_clk), .resetn(common_reset_n), .core_clk(ui_clk), .ddr_init_done(init_done),
        .axi_awaddr(axi_awaddr), .axi_awuser_ap(1'b0), .axi_awuser_id(4'b0), .axi_awlen(4'b0),
        .axi_awready(axi_awready), .axi_awvalid(axi_awvalid), .axi_wdata(axi_wdata),
        .axi_wstrb(axi_wstrb), .axi_wready(axi_wready),
        .axi_wusero_id(axi_wusero_id), .axi_wusero_last(axi_wusero_last),
        .axi_araddr(axi_araddr), .axi_aruser_ap(1'b0), .axi_aruser_id(4'b0), .axi_arlen(4'b0),
        .axi_arready(axi_arready), .axi_arvalid(axi_arvalid), .axi_rdata(axi_rdata),
        .axi_rid(axi_rid), .axi_rlast(axi_rlast), .axi_rvalid(axi_rvalid),
        .apb_clk(1'b0), .apb_rst_n(1'b0), .apb_sel(1'b0), .apb_enable(1'b0),
        .apb_addr(8'b0), .apb_write(1'b0), .apb_wdata(16'b0),
        .mem_cs_n(mem_cs_n), .mem_rst_n(mem_rst_n), .mem_ck(mem_ck), .mem_ck_n(mem_ck_n),
        .mem_cke(mem_cke), .mem_ras_n(mem_ras_n), .mem_cas_n(mem_cas_n), .mem_we_n(mem_we_n),
        .mem_odt(mem_odt), .mem_a(mem_a), .mem_ba(mem_ba), .mem_dqs(mem_dqs),
        .mem_dqs_n(mem_dqs_n), .mem_dq(mem_dq), .mem_dm(mem_dm),
        .dbg_gate_start(1'b0), .dbg_cpd_start(1'b0), .dbg_ddrphy_rst_n(1'b1), .dbg_gpll_scan_rst(1'b0),
        .samp_position_dyn_adj(1'b0), .init_samp_position_even(32'b0), .init_samp_position_odd(32'b0),
        .wrcal_position_dyn_adj(1'b0), .init_wrcal_position(32'b0), .force_read_clk_ctrl(1'b0),
        .init_slip_step(16'b0), .init_read_clk_ctrl(12'b0), .debug_cpd_offset_adj(1'b0),
        .debug_cpd_offset_dir(1'b0), .debug_cpd_offset(10'b0), .ck_dly_en(1'b1), .init_ck_dly_step(8'b0)
    );
endmodule
