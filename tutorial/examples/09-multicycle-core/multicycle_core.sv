// Teaching bridge between the toy CPU and the project core.
// Reuses the real decoder/ALU; supports ordinary RV32I arithmetic, branches,
// jumps and aligned loads/stores only. Unsupported operations halt with fault.
module multicycle_core (
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
    output logic [4:0] retire_rd,
    output logic fault
);
    import rv32_pkg::*;
    localparam [2:0] IF_REQ=0, IF_RSP=1, EXEC=2, D_REQ=3, D_RSP=4, HALT=5;
    logic [2:0] state;
    logic [31:0] pc, instruction, regs[0:31];
    decode_t dec;
    logic [31:0] a,b,alu_a,alu_b,alu_result,addr,target,next_pc,shifted,load_result;
    logic taken,misaligned;
    logic [31:0] pending_pc,pending_insn,pending_addr,pending_data,pending_next;
    logic [4:0] pending_rd;
    logic [2:0] pending_funct3;
    logic pending_load,pending_store;
    rv32_decode decoder(.insn(instruction),.decoded(dec));
    rv32_alu alu(.op(dec.alu),.a(alu_a),.b(alu_b),.result(alu_result));
    always_comb begin
        a = dec.rs1 == 0 ? 0 : regs[dec.rs1];
        b = dec.rs2 == 0 ? 0 : regs[dec.rs2];
        alu_a = dec.src_pc ? pc : a;
        alu_b = dec.src_imm ? dec.imm : b;
        addr = a + dec.imm;
        taken = dec.jal || dec.jalr;
        if(dec.branch) begin
            case(dec.funct3)
                0: taken=a==b;
                1: taken=a!=b;
                4: taken=$signed(a)<$signed(b);
                5: taken=$signed(a)>=$signed(b);
                6: taken=a<b;
                7: taken=a>=b;
                default: taken=0;
            endcase
        end
        target=dec.jalr ? ((a+dec.imm)&32'hfffffffe) : pc+dec.imm;
        next_pc=taken ? target : pc+4;
        misaligned=(dec.funct3[1:0]==1 && addr[0]) ||
                   (dec.funct3[1:0]==2 && addr[1:0]!=0);
        shifted=d_rsp_data >> {pending_addr[1:0],3'b0};
        case(pending_funct3)
            0: load_result={{24{shifted[7]}},shifted[7:0]};
            1: load_result={{16{shifted[15]}},shifted[15:0]};
            4: load_result={24'b0,shifted[7:0]};
            5: load_result={16'b0,shifted[15:0]};
            default: load_result=d_rsp_data;
        endcase
    end
    assign i_req_valid=(state==IF_REQ) && !rst;
    assign i_req_addr=pc;
    assign i_rsp_ready=(state==IF_RSP) && !rst;
    assign d_req_valid=(state==D_REQ) && !rst;
    assign d_req_addr={pending_addr[31:2],2'b0};
    assign d_req_write=pending_store;
    assign d_req_wdata=pending_data << {pending_addr[1:0],3'b0};
    always_comb begin
        case(pending_funct3[1:0])
            0: d_req_wstrb=4'b0001 << pending_addr[1:0];
            1: d_req_wstrb=4'b0011 << pending_addr[1:0];
            default: d_req_wstrb=4'b1111;
        endcase
    end
    assign d_rsp_ready=(state==D_RSP) && !rst;
    always_ff @(posedge clk) begin
        if(rst) begin
            state<=IF_REQ; pc<=0; instruction<=0; fault<=0;
            retire_valid<=0; retire_pc<=0; retire_insn<=0; retire_rd<=0; retire_data<=0;
            pending_pc<=0; pending_insn<=0; pending_addr<=0; pending_data<=0;
            pending_next<=0; pending_rd<=0; pending_funct3<=0;
            pending_load<=0; pending_store<=0;
        end else begin
            retire_valid<=0;
            case(state)
                IF_REQ: if(i_req_valid && i_req_ready) state<=IF_RSP;
                IF_RSP: if(i_rsp_valid && i_rsp_ready) begin
                    if(i_rsp_error[0]) begin fault<=1; state<=HALT; end
                    else begin instruction<=i_rsp_data[31:0]; state<=EXEC; end
                end
                EXEC: begin
                    if(dec.illegal || dec.mul || dec.div || dec.csr || dec.ecall ||
                       dec.ebreak || dec.mret || dec.fence_i || dec.serial ||
                       (taken && target[1:0]!=0) ||
                       ((dec.load || dec.store) && misaligned)) begin
                        fault<=1; state<=HALT;
                    end else if(dec.load || dec.store) begin
                        pending_pc<=pc; pending_insn<=instruction; pending_addr<=addr;
                        pending_data<=b; pending_next<=pc+4; pending_rd<=dec.rd;
                        pending_funct3<=dec.funct3; pending_load<=dec.load; pending_store<=dec.store;
                        state<=D_REQ;
                    end else begin
                        retire_valid<=1; retire_pc<=pc; retire_insn<=instruction;
                        retire_rd<=dec.reg_write ? dec.rd : 0;
                        retire_data<=(dec.reg_write && dec.rd!=0) ?
                            ((dec.jal || dec.jalr) ? pc+4 : alu_result) : 0;
                        if(dec.reg_write && dec.rd!=0)
                            regs[dec.rd] <= (dec.jal || dec.jalr) ? pc+4 : alu_result;
                        pc<=next_pc; state<=IF_REQ;
                    end
                end
                D_REQ: if(d_req_valid && d_req_ready) state<=D_RSP;
                D_RSP: if(d_rsp_valid && d_rsp_ready) begin
                    if(d_rsp_error) begin fault<=1; state<=HALT; end
                    else begin
                        retire_valid<=1; retire_pc<=pending_pc; retire_insn<=pending_insn;
                        retire_rd<=pending_load ? pending_rd : 0;
                        retire_data<=(pending_load && pending_rd!=0) ? load_result : 0;
                        if(pending_load && pending_rd!=0) regs[pending_rd]<=load_result;
                        pc<=pending_next; state<=IF_REQ;
                    end
                end
                default: state<=HALT;
            endcase
        end
    end
endmodule
