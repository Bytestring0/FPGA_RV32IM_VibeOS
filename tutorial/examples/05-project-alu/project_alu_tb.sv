`timescale 1ns/1ps
module project_alu_tb;
    import rv32_pkg::*;
    logic [4:0] op;
    logic [31:0] a, b;
    wire [31:0] result;
    rv32_alu dut(.op(op),.a(a),.b(b),.result(result));
    integer cases=0;
    task automatic check(input logic [4:0] fn, input logic [31:0] x,y,expected);
        begin
            op=fn; a=x; b=y; #1;
            if (result !== expected) $fatal(1,"op=%0d a=%h b=%h got=%h expected=%h",fn,x,y,result,expected);
            cases=cases+1;
        end
    endtask
    initial begin
        check(ALU_ADD,32'hffffffff,2,1);
        check(ALU_SUB,0,1,32'hffffffff);
        check(ALU_SLT,32'hffffffff,1,1);
        check(ALU_SLTU,32'hffffffff,1,0);
        check(ALU_SRA,32'h80000000,4,32'hf8000000);
        check(ALU_SRL,32'h80000000,4,32'h08000000);
        check(ALU_SLL,1,33,2);
        check(ALU_ROR,32'h12345678,8,32'h78123456);
        check(ALU_ROL,32'h12345678,8,32'h34567812);
        check(ALU_CLZ,0,0,32);
        check(ALU_CTZ,0,0,32);
        check(ALU_CPOP,32'hf00000ff,0,12);
        check(ALU_SEXTB,32'h80,0,32'hffffff80);
        check(ALU_REV8,32'h12345678,0,32'h78563412);
        check(ALU_SH2ADD,3,5,17);
        $display("PASS project_alu: %0d directed cases",cases);
        $finish;
    end
endmodule
