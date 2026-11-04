`timescale 1ns/1ps
module fetch_queue_tb;
    logic clk=0,rst=1,flush=0,push_valid=0;
    logic [1:0] push_count=0,push_error=0,pop_count=0;
    logic [31:0] push_pc=0,push_insn0=0,push_insn1=0;
    wire push_ready;
    wire [2:0] count;
    wire [31:0] pc0,insn0,pc1,insn1;
    wire error0,error1;
    fetch_queue dut(.*);
    always #5 clk=~clk;
    logic [31:0] model_pc[0:3],model_insn[0:3];
    logic model_error[0:3];
    integer n=0,i,k,pop,do_push,cycles,flushes=0,overlap=0;
    initial begin
        repeat(2) @(negedge clk); rst=0;
        for(cycles=0;cycles<5000;cycles=cycles+1) begin
            @(negedge clk);
            flush = ($urandom_range(0,29)==0);
            pop = n==0 ? 0 : $urandom_range(0,(n<2?n:2));
            pop_count=pop;
            push_valid=$urandom_range(0,1);
            push_count=$urandom_range(1,2);
            push_pc=cycles*8; push_insn0=32'h10000000+cycles;
            push_insn1=32'h20000000+cycles;
            push_error=$urandom_range(0,3);
            #1; do_push=push_valid && push_ready;
            if(flush) begin n=0; flushes=flushes+1; end
            else begin
                if(pop!=0 && do_push) overlap=overlap+1;
                for(i=0;i<n-pop;i=i+1) begin
                    model_pc[i]=model_pc[i+pop]; model_insn[i]=model_insn[i+pop];
                    model_error[i]=model_error[i+pop];
                end
                n=n-pop;
                if(do_push) begin
                    model_pc[n]=push_pc; model_insn[n]=push_insn0; model_error[n]=push_error[0]; n=n+1;
                    if(push_count==2) begin
                        model_pc[n]=push_pc+4; model_insn[n]=push_insn1; model_error[n]=push_error[1]; n=n+1;
                    end
                end
            end
            @(posedge clk); #1;
            if(count !== n) $fatal(1,"cycle=%0d count=%0d expected=%0d",cycles,count,n);
            if(n>0 && {pc0,insn0,error0} !== {model_pc[0],model_insn[0],model_error[0]})
                $fatal(1,"cycle=%0d first item mismatch",cycles);
            if(n>1 && {pc1,insn1,error1} !== {model_pc[1],model_insn[1],model_error[1]})
                $fatal(1,"cycle=%0d second item mismatch",cycles);
        end
        if(flushes==0 || overlap==0) $fatal(1,"coverage missing");
        $display("PASS fetch_queue: %0d cycles, %0d flushes, %0d simultaneous pop/push",cycles,flushes,overlap);
        $finish;
    end
endmodule
