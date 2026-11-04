// Teaching stage 1: execute ADDI only. Not a complete RISC-V processor.
module addi_cpu (
    input  logic        clk,
    input  logic        rst,
    input  logic [31:0] insn,
    output logic [31:0] pc,
    output logic        halted
);
    logic [31:0] regs [0:31];
    wire [4:0] rd  = insn[11:7];
    wire [4:0] rs1 = insn[19:15];
    wire [31:0] imm = {{20{insn[31]}}, insn[31:20]};
    wire [31:0] a = (rs1 == 0) ? 32'b0 : regs[rs1];
    wire is_addi = (insn[6:0] == 7'h13) && (insn[14:12] == 3'b000);
    integer i;

    always_ff @(posedge clk) begin
        if (rst) begin
            pc <= 0;
            halted <= 0;
            // Teaching convenience, not a requirement of the RISC-V ISA.
            for (i = 0; i < 32; i = i + 1) regs[i] <= 0;
        end else if (!halted) begin
            if (is_addi) begin
                if (rd != 0) regs[rd] <= a + imm;
                pc <= pc + 32'd4;
            end else begin
                halted <= 1;
            end
        end
    end
endmodule
