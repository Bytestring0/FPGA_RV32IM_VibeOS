`timescale 1ns/1ps
module addi_tb;
    logic clk = 0, rst = 1;
    wire [31:0] pc;
    wire halted;
    logic [31:0] insn;
    logic [31:0] rom [0:4];

    addi_cpu dut(.clk(clk), .rst(rst), .insn(insn), .pc(pc), .halted(halted));
    always #5 clk = ~clk;
    always_comb begin
        if (pc[1:0] != 0 || pc >= 20) insn = 32'hffffffff;
        else insn = rom[pc[31:2]];
    end

    initial begin
        rom[0] = 32'h00700293; // addi x5,x0,7
        rom[1] = 32'hffd28313; // addi x6,x5,-3
        rom[2] = 32'h00130013; // addi x0,x6,1 -- ignored
        rom[3] = 32'h00100393; // addi x7,x0,1
        rom[4] = 32'hffffffff; // unsupported: stop
        repeat (2) @(negedge clk);
        rst = 0;
        wait (halted);
        #1;
        if (pc !== 32'd16 || dut.regs[5] !== 32'd7 ||
            dut.regs[6] !== 32'd4 || dut.regs[7] !== 32'd1 || dut.regs[0] !== 0)
            $fatal(1, "unexpected final state");
        $display("PASS addi_cpu: x5=%0d x6=%0d x7=%0d pc=%0d",
                 dut.regs[5], dut.regs[6], dut.regs[7], pc);
        $finish;
    end
    always @(posedge clk) if (!rst && !halted)
        $display("pc=%0d instruction=%08x", pc, insn);
    initial begin #500; $fatal(1, "timeout"); end
endmodule
