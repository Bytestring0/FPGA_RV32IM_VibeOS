`timescale 1ns/1ps
module project_decode_tb;
    import rv32_pkg::*;
    logic [31:0] insn;
    wire [86:0] decoded;
    decode_t d;
    assign d = decoded;
    rv32_decode dut(.insn(insn), .decoded(decoded));
    initial begin
        // ADDI x5,x0,-3
        insn=32'hffd00293; #1;
        if (d.illegal || !d.reg_write || !d.use_rs1 || d.use_rs2 ||
            d.rd != 5 || d.rs1 != 0 || d.imm !== 32'hfffffffd || d.alu != ALU_ADD)
            $fatal(1,"ADDI decode failed");
        // SW x5,8(x1)
        insn=32'h0050a423; #1;
        if (d.illegal || !d.store || d.reg_write || !d.use_rs1 || !d.use_rs2 ||
            d.rs1 != 1 || d.rs2 != 5 || d.imm !== 8)
            $fatal(1,"SW decode failed");
        // BEQ x1,x2,+8
        insn=32'h00208463; #1;
        if (d.illegal || !d.branch || !d.use_rs1 || !d.use_rs2 || d.imm !== 8)
            $fatal(1,"BEQ decode failed");
        // SUB x7,x5,x6
        insn=32'h406283b3; #1;
        if (d.illegal || !d.reg_write || d.alu != ALU_SUB || d.rd != 7 ||
            d.rs1 != 5 || d.rs2 != 6) $fatal(1,"SUB decode failed");
        // MUL x7,x5,x6
        insn=32'h026283b3; #1;
        if (d.illegal || !d.mul || d.div || !d.reg_write) $fatal(1,"MUL decode failed");
        // CSRRW x1,mstatus,x2
        insn=32'h300110f3; #1;
        if (d.illegal || !d.csr || !d.serial || !d.use_rs1 || d.csr_imm ||
            d.csr_addr != 12'h300 || d.rd != 1 || d.rs1 != 2)
            $fatal(1,"CSR decode failed");
        insn=32'hffffffff; #1;
        if (!d.illegal) $fatal(1,"unknown opcode accepted");
        $display("PASS project_decode: 7 directed cases");
        $finish;
    end
endmodule
