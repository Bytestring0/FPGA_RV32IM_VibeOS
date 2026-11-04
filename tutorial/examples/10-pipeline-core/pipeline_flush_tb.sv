`timescale 1ns/1ps
module pipeline_flush_tb;
    logic clk=0,rst=1;
    wire i_req_valid,i_rsp_ready,d_req_valid,d_req_write,d_rsp_ready;
    logic i_req_ready=1,i_rsp_valid=0,d_req_ready=1,d_rsp_valid=0;
    wire [31:0] i_req_addr,d_req_addr,d_req_wdata;
    wire [3:0] d_req_wstrb;
    logic [63:0] i_rsp_data=0;
    logic [1:0] i_rsp_error=0;
    logic [31:0] d_rsp_data=0;
    logic d_rsp_error=0;
    wire retire_valid,fault;
    wire [31:0] retire_pc,retire_insn,retire_data;
    wire [4:0] retire_rd;
    logic [31:0] rom[0:31];
    integer scenario=0,retired=0,stores=0,dwait=-1,wrong_i_accepted=0,older_load_younger_store_seen=0;
    pipeline_core dut(.*);
    always #5 clk=~clk;
    always @(posedge clk) begin
        if(rst) begin i_rsp_valid<=0; d_rsp_valid<=0; dwait<=-1; retired<=0; stores<=0; wrong_i_accepted<=0; older_load_younger_store_seen<=0; end
        else begin
            if(scenario==1 && dut.m_valid && dut.m_pc==4 && dut.x_valid && dut.x_pc==8)
                older_load_younger_store_seen<=older_load_younger_store_seen+1;
            if(i_rsp_valid && i_rsp_ready) i_rsp_valid<=0;
            if(i_req_valid && i_req_ready) begin
                if(i_req_addr>=128) $fatal(1,"bad fetch");
                if(scenario==0 && (i_req_addr==8 || i_req_addr==12)) wrong_i_accepted<=wrong_i_accepted+1;
                i_rsp_data<={32'b0,rom[i_req_addr>>2]}; i_rsp_error<=0; i_rsp_valid<=1;
            end
            if(d_rsp_valid && d_rsp_ready) d_rsp_valid<=0;
            if(dwait>0) dwait<=dwait-1;
            else if(dwait==0) begin d_rsp_valid<=1; dwait<=-1; end
            if(d_req_valid && d_req_ready) begin
                if(d_req_write) stores<=stores+1;
                d_rsp_error<=(scenario==1); dwait<=12;
            end
            if(retire_valid) begin
                if(scenario==0) begin
                    case(retired)
                        0: if(retire_pc!==0 || retire_rd!==1 || retire_data!==0) $fatal(1,"first retirement");
                        1: if(retire_pc!==4 || retire_rd!==0) $fatal(1,"branch retirement");
                        2: if(retire_pc!==16 || retire_rd!==2 || retire_data!==7) $fatal(1,"target retirement");
                        default: $fatal(1,"unexpected retirement");
                    endcase
                end else if(retired!=0 || retire_pc!==0) $fatal(1,"younger retirement after fault");
                retired<=retired+1;
            end
        end
    end
    integer k;
    initial begin
        for(k=0;k<32;k=k+1) rom[k]=32'hffffffff;
        rom[0]=32'h00002083; // lw x1,0(x0), delayed success
        rom[1]=32'h00000663; // beq x0,x0,+12 (to PC 16)
        rom[2]=32'h00102023; // wrong-path sw x1,0(x0)
        rom[3]=32'h06300193; // wrong-path addi x3,x0,99
        rom[4]=32'h00700113; // addi x2,x0,7
        repeat(2) @(negedge clk); rst=0;
        wait(fault); repeat(4) @(negedge clk);
        if(retired!==3 || stores!==0 || wrong_i_accepted==0)
            $fatal(1,"redirect scenario missing coverage or wrong result retire=%0d store=%0d wrong_fetch=%0d",retired,stores,wrong_i_accepted);
        @(negedge clk); rst=1; scenario=1;
        for(k=0;k<32;k=k+1) rom[k]=32'hffffffff;
        rom[0]=32'h00000093; // addi x1,x0,0
        rom[1]=32'h0000a103; // lw x2,0(x1), simulated error
        rom[2]=32'h00102223; // younger sw x1,4(x0) must not be accepted
        rom[3]=32'h00500193; // younger addi
        repeat(3) @(negedge clk); rst=0;
        wait(fault); repeat(4) @(negedge clk);
        if(retired!==1 || stores!==0 || older_load_younger_store_seen==0)
            $fatal(1,"older memory error scenario retire=%0d stores=%0d overlap=%0d",retired,stores,older_load_younger_store_seen);
        $display("PASS pipeline_flush: wrong-path store suppressed; older load error suppresses younger store");
        $finish;
    end
    initial begin #20000; $fatal(1,"timeout"); end
endmodule
