`timescale 1ns/1ps
module small_tb;
    logic clk = 0, rst = 1;
    wire [31:0] pc, mem_addr, mem_wdata;
    wire mem_we, halted;
    logic [31:0] insn, mem_rdata;
    logic [31:0] rom [0:13];
    logic [31:0] ram [0:15];
    integer stores = 0;
    small_cpu dut(.*);
    always #5 clk = ~clk;
    always_comb begin
        insn = (pc[1:0] == 0 && pc < 56) ? rom[pc[31:2]] : 32'hffffffff;
        mem_rdata = (mem_addr[1:0] == 0 && mem_addr < 64) ? ram[mem_addr[31:2]] : 0;
    end
    always @(posedge clk) begin
        if (mem_we) begin
            if (mem_addr[1:0] != 0 || mem_addr >= 64) $fatal(1, "bad store address");
            ram[mem_addr[31:2]] <= mem_wdata;
            stores <= stores + 1;
        end
        if (!rst && !halted) $display("pc=%02x insn=%08x", pc, insn);
    end
    initial begin
        rom[0]=32'h00000093; rom[1]=32'h00500113;
        rom[2]=32'h002080b3; rom[3]=32'hfff10113;
        rom[4]=32'hfe011ce3; rom[5]=32'h00102023;
        rom[6]=32'h00002183; rom[7]=32'h40118233;
        rom[8]=32'h00020463; rom[9]=32'h06300393;
        rom[10]=32'h008002ef; rom[11]=32'h06200393;
        rom[12]=32'h00028313; rom[13]=32'hffffffff;
        repeat (2) @(negedge clk);
        rst = 0;
        wait (halted);
        #1;
        if (pc !== 52 || dut.regs[1] !== 15 || dut.regs[2] !== 0 ||
            dut.regs[3] !== 15 || dut.regs[4] !== 0 || dut.regs[5] !== 44 ||
            dut.regs[6] !== 44 || dut.regs[7] !== 0 || ram[0] !== 15 || stores !== 1)
            $fatal(1, "wrong final result or control flow");
        $display("PASS small_cpu: sum=%0d stores=%0d link=%0d", ram[0], stores, dut.regs[5]);
        $finish;
    end
    initial begin #1000; $fatal(1, "timeout"); end
endmodule
