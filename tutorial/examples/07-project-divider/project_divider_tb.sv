`timescale 1ns/1ps
module project_divider_tb;
    logic clk=0,rst=1,cancel=0,start=0;
    logic [31:0] a=0,b=0;
    logic [2:0] funct3=0;
    wire busy,done;
    wire [31:0] result;
    integer cases=0;
    rv32_divider dut(.*);
    always #5 clk=~clk;
    task automatic check(input logic [2:0] f,input logic [31:0] x,y,exp);
        begin
            @(negedge clk); a=x; b=y; funct3=f; start=1;
            @(negedge clk); start=0;
            while (!done) @(negedge clk);
            if (result !== exp) $fatal(1,"f=%0d a=%h b=%h got=%h exp=%h",f,x,y,result,exp);
            cases=cases+1;
        end
    endtask
    initial begin
        repeat(2) @(negedge clk); rst=0;
        check(3'b100,32'hfffffff9,3,32'hfffffffe); // DIV -7/3 = -2
        check(3'b110,32'hfffffff9,3,32'hffffffff); // REM -7%3 = -1
        check(3'b101,32'hffffffff,2,32'h7fffffff); // DIVU
        check(3'b111,32'hffffffff,2,1);             // REMU
        check(3'b100,32'hfffffffb,0,32'hffffffff); // DIV by zero
        check(3'b110,32'hfffffffb,0,32'hfffffffb); // REM by zero
        check(3'b100,32'h80000000,32'hffffffff,32'h80000000);
        check(3'b110,32'h80000000,32'hffffffff,0);
        @(negedge clk); a=100; b=3; funct3=3'b101; start=1;
        @(negedge clk); start=0; repeat(4) @(negedge clk);
        cancel=1; @(negedge clk); cancel=0;
        if (busy || done) $fatal(1,"cancel failed");
        check(3'b101,100,3,33);
        $display("PASS project_divider: %0d cases and cancel",cases);
        $finish;
    end
    initial begin #10000; $fatal(1,"timeout"); end
endmodule
