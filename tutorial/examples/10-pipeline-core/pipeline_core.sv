// Teaching pipeline: one issue, conservative RAW stalls, static PC+4 fetch.
// Uses project decoder/ALU. Unsupported instructions/errors set fault; no CSR trap.
module pipeline_core (
    input logic clk, rst,
    output logic i_req_valid, input logic i_req_ready,
    output logic [31:0] i_req_addr,
    input logic i_rsp_valid, output logic i_rsp_ready,
    input logic [63:0] i_rsp_data, input logic [1:0] i_rsp_error,
    output logic d_req_valid, input logic d_req_ready,
    output logic [31:0] d_req_addr, d_req_wdata,
    output logic [3:0] d_req_wstrb, output logic d_req_write,
    input logic d_rsp_valid, output logic d_rsp_ready,
    input logic [31:0] d_rsp_data, input logic d_rsp_error,
    output logic retire_valid, output logic [31:0] retire_pc, retire_insn, retire_data,
    output logic [4:0] retire_rd, output logic fault
);
    import rv32_pkg::*;
    logic [31:0] regs[0:31];
    logic [31:0] f_pc, q_pc;
    logic q_valid, q_killed, f_valid, f_error;
    logic [31:0] f_insn, f_insn_pc;
    decode_t f_dec, x_dec;
    logic x_valid, x_error;
    logic [31:0] x_pc, x_insn, x_a, x_b;
    logic m_valid, m_sent, m_load, m_store, m_write;
    logic [31:0] m_pc, m_insn, m_addr, m_data, m_result;
    logic [4:0] m_rd;
    logic [2:0] m_funct3;
    logic w_valid, w_write;
    logic [31:0] w_pc, w_insn, w_result;
    logic [4:0] w_rd;
    logic [31:0] alu_a, alu_b, alu_result, addr, target, next_pc, x_result;
    logic [31:0] shifted, mem_result;
    logic taken, misaligned, x_fault, hazard, x_go, x_ready, m_finish, m_ready;
    logic issue, branch_redirect, mem_error, fault_now, flush;
    wire i_accept=i_req_valid && i_req_ready;
    wire i_consume=i_rsp_valid && i_rsp_ready;
    wire d_accept=d_req_valid && d_req_ready;
    rv32_decode dec_front(.insn(f_insn),.decoded(f_dec));
    rv32_alu alu(.op(x_dec.alu),.a(alu_a),.b(alu_b),.result(alu_result));
    always_comb begin
        alu_a=x_dec.src_pc ? x_pc : x_a;
        alu_b=x_dec.src_imm ? x_dec.imm : x_b;
        addr=x_a+x_dec.imm;
        taken=x_dec.jal || x_dec.jalr;
        if(x_dec.branch) begin
            case(x_dec.funct3)
                0: taken=x_a==x_b;
                1: taken=x_a!=x_b;
                4: taken=$signed(x_a)<$signed(x_b);
                5: taken=$signed(x_a)>=$signed(x_b);
                6: taken=x_a<x_b;
                7: taken=x_a>=x_b;
                default: taken=0;
            endcase
        end
        target=x_dec.jalr ? ((x_a+x_dec.imm)&32'hfffffffe) : x_pc+x_dec.imm;
        next_pc=taken ? target : x_pc+4;
        x_result=(x_dec.jal || x_dec.jalr) ? x_pc+4 : alu_result;
        misaligned=(x_dec.funct3[1:0]==1 && addr[0]) ||
                   (x_dec.funct3[1:0]==2 && addr[1:0]!=0);
        x_fault=x_error || x_dec.illegal || x_dec.mul || x_dec.div || x_dec.csr ||
                x_dec.ecall || x_dec.ebreak || x_dec.mret || x_dec.fence_i || x_dec.serial ||
                (taken && target[1:0]!=0) || ((x_dec.load || x_dec.store) && misaligned);
        shifted=d_rsp_data >> {m_addr[1:0],3'b0};
        case(m_funct3)
            0: mem_result={{24{shifted[7]}},shifted[7:0]};
            1: mem_result={{16{shifted[15]}},shifted[15:0]};
            4: mem_result={24'b0,shifted[7:0]};
            5: mem_result={16'b0,shifted[15:0]};
            default: mem_result=d_rsp_data;
        endcase
        if(!m_load) mem_result=m_result;
    end
    function automatic logic reads_rd(input decode_t dec,input logic [4:0] rd);
        reads_rd = rd!=0 && ((dec.use_rs1 && dec.rs1==rd) || (dec.use_rs2 && dec.rs2==rd));
    endfunction
    always_comb begin
        hazard=(x_valid && x_dec.reg_write && reads_rd(f_dec,x_dec.rd)) ||
               (m_valid && m_write && reads_rd(f_dec,m_rd)) ||
               (w_valid && w_write && reads_rd(f_dec,w_rd));
    end
    assign d_req_valid=m_valid && (m_load || m_store) && !m_sent && !rst && !fault;
    assign d_req_addr={m_addr[31:2],2'b0};
    assign d_req_write=m_store;
    assign d_req_wdata=m_data << {m_addr[1:0],3'b0};
    always_comb begin
        case(m_funct3[1:0])
            0: d_req_wstrb=4'b0001 << m_addr[1:0];
            1: d_req_wstrb=4'b0011 << m_addr[1:0];
            default: d_req_wstrb=4'b1111;
        endcase
    end
    assign d_rsp_ready=m_valid && m_sent && !rst;
    assign m_finish=m_valid && (!(m_load || m_store) || (d_rsp_valid && d_rsp_ready));
    assign mem_error=m_finish && (m_load || m_store) && d_rsp_error;
    assign m_ready=!m_valid || m_finish;
    assign x_go=x_valid && m_ready && !mem_error && !fault;
    assign x_ready=!x_valid || x_go;
    assign branch_redirect=x_go && !x_fault && next_pc!=x_pc+4;
    assign fault_now=mem_error || (x_go && x_fault);
    assign flush=branch_redirect || fault_now;
    assign issue=f_valid && x_ready && !hazard && !flush && !fault;
    assign i_req_valid=!q_valid && !f_valid && !flush && !fault && !rst;
    assign i_req_addr=f_pc;
    assign i_rsp_ready=q_valid && (q_killed || !f_valid || flush || fault) && !rst;
    assign retire_valid=w_valid;
    assign retire_pc=w_pc;
    assign retire_insn=w_insn;
    assign retire_rd=w_write ? w_rd : 0;
    assign retire_data=(w_write && w_rd!=0) ? w_result : 0;
    always_ff @(posedge clk) begin
        if(rst) begin
            f_pc<=0; q_pc<=0; q_valid<=0; q_killed<=0;
            f_valid<=0; f_error<=0; f_insn<=0; f_insn_pc<=0;
            x_valid<=0; x_error<=0; x_pc<=0; x_insn<=0; x_a<=0; x_b<=0; x_dec<='0;
            m_valid<=0; m_sent<=0; m_load<=0; m_store<=0; m_write<=0;
            m_pc<=0; m_insn<=0; m_addr<=0; m_data<=0; m_result<=0; m_rd<=0; m_funct3<=0;
            w_valid<=0; w_write<=0; w_pc<=0; w_insn<=0; w_result<=0; w_rd<=0;
            fault<=0;
        end else begin
            if(w_valid && w_write && w_rd!=0) regs[w_rd]<=w_result;
            if(i_consume) begin
                q_valid<=0; q_killed<=0;
                if(!q_killed && !flush && !fault) begin
                    f_valid<=1; f_insn_pc<=q_pc; f_insn<=i_rsp_data[31:0]; f_error<=i_rsp_error[0];
                end
            end
            if(i_accept) begin q_valid<=1; q_killed<=0; q_pc<=f_pc; f_pc<=f_pc+4; end
            if(issue) f_valid<=0;
            if(flush) begin
                f_valid<=0;
                if(q_valid && !i_consume) q_killed<=1;
                if(branch_redirect) f_pc<=next_pc;
            end
            if(x_ready) begin
                x_valid<=issue;
                if(issue) begin
                    x_pc<=f_insn_pc; x_insn<=f_insn; x_error<=f_error; x_dec<=f_dec;
                    x_a<=f_dec.rs1==0 ? 0 : regs[f_dec.rs1];
                    x_b<=f_dec.rs2==0 ? 0 : regs[f_dec.rs2];
                end
            end
            if(fault_now) begin fault<=1; x_valid<=0; end
            if(m_ready) begin
                m_valid<=x_go && !x_fault && !mem_error;
                m_sent<=0;
                if(x_go) begin
                    m_pc<=x_pc; m_insn<=x_insn; m_rd<=x_dec.rd;
                    m_addr<=addr; m_data<=x_b; m_result<=x_result;
                    m_funct3<=x_dec.funct3; m_load<=x_dec.load; m_store<=x_dec.store;
                    m_write<=x_dec.reg_write;
                end
            end else if(d_accept) m_sent<=1;
            w_valid<=m_finish && !mem_error;
            if(m_finish && !mem_error) begin
                w_pc<=m_pc; w_insn<=m_insn; w_rd<=m_rd;
                w_result<=mem_result; w_write<=m_write;
            end
        end
    end
endmodule
