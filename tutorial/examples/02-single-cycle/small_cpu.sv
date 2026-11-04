// Teaching stage 2: ADDI, ADD/SUB, LUI, LW/SW, BEQ/BNE, JAL.
// Instruction/data reads are combinational; no bus errors or architectural traps.
module small_cpu (
    input  logic        clk, rst,
    input  logic [31:0] insn,
    output logic [31:0] pc,
    output logic [31:0] mem_addr, mem_wdata,
    output logic        mem_we,
    input  logic [31:0] mem_rdata,
    output logic        halted
);
    logic [31:0] regs [0:31];
    wire [6:0] op = insn[6:0];
    wire [2:0] f3 = insn[14:12];
    wire [6:0] f7 = insn[31:25];
    wire [4:0] rd = insn[11:7], rs1 = insn[19:15], rs2 = insn[24:20];
    wire [31:0] a = rs1 == 0 ? 32'b0 : regs[rs1];
    wire [31:0] b = rs2 == 0 ? 32'b0 : regs[rs2];
    wire [31:0] imm_i = {{20{insn[31]}}, insn[31:20]};
    wire [31:0] imm_s = {{20{insn[31]}}, insn[31:25], insn[11:7]};
    wire [31:0] imm_b = {{19{insn[31]}}, insn[31], insn[7],
                         insn[30:25], insn[11:8], 1'b0};
    wire [31:0] imm_j = {{11{insn[31]}}, insn[31], insn[19:12],
                         insn[20], insn[30:21], 1'b0};
    logic [31:0] next_pc, result;
    logic write_rd, bad;
    integer i;

    always_comb begin
        next_pc = pc + 32'd4;
        result = 0;
        write_rd = 0;
        bad = 0;
        mem_addr = 0;
        mem_wdata = b;
        mem_we = 0;
        case (op)
            7'h37: begin // LUI
                result = {insn[31:12], 12'b0}; write_rd = 1;
            end
            7'h13: begin // ADDI only
                if (f3 == 0) begin result = a + imm_i; write_rd = 1; end
                else bad = 1;
            end
            7'h33: begin // ADD/SUB only
                if (f3 == 0 && (f7 == 0 || f7 == 7'h20)) begin
                    result = f7 == 0 ? a + b : a - b; write_rd = 1;
                end else bad = 1;
            end
            7'h03: begin // LW only
                mem_addr = a + imm_i;
                if (f3 == 2 && mem_addr[1:0] == 0) begin
                    result = mem_rdata; write_rd = 1;
                end else bad = 1;
            end
            7'h23: begin // SW only
                mem_addr = a + imm_s;
                if (f3 == 2 && mem_addr[1:0] == 0) mem_we = 1;
                else bad = 1;
            end
            7'h63: begin // BEQ/BNE only
                if (f3 != 0 && f3 != 1) bad = 1;
                else if ((f3 == 0 && a == b) || (f3 == 1 && a != b)) begin
                    next_pc = pc + imm_b;
                    if (next_pc[1:0] != 0) bad = 1;
                end
            end
            7'h6f: begin // JAL
                next_pc = pc + imm_j;
                if (next_pc[1:0] != 0) bad = 1;
                else begin result = pc + 32'd4; write_rd = 1; end
            end
            default: bad = 1;
        endcase
        if (rst || halted || bad) mem_we = 0;
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            pc <= 0; halted <= 0;
            for (i = 0; i < 32; i = i + 1) regs[i] <= 0;
        end else if (!halted) begin
            if (bad) halted <= 1;
            else begin
                if (write_rd && rd != 0) regs[rd] <= result;
                pc <= next_pc;
            end
        end
    end
endmodule
