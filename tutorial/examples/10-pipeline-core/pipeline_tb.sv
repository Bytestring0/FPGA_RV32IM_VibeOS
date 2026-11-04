`timescale 1ns/1ps
module pipeline_tb;
    logic clk=0,rst=1;
    wire i_req_valid,i_rsp_ready,d_req_valid,d_req_write,d_rsp_ready;
    logic i_req_ready,i_rsp_valid=0,d_req_ready,d_rsp_valid=0;
    wire [31:0] i_req_addr,d_req_addr,d_req_wdata;
    wire [3:0] d_req_wstrb;
    logic [63:0] i_rsp_data=0;
    logic [1:0] i_rsp_error=0;
    logic [31:0] d_rsp_data=0;
    logic d_rsp_error=0;
    wire retire_valid,fault;
    wire [31:0] retire_pc,retire_insn,retire_data;
    wire [4:0] retire_rd;
    logic [31:0] rom[0:13],ram[0:15];
    integer cycle=0,iwait=-1,dwait=-1,stores=0,retired=0,j;
    logic [31:0] expected_pc[0:22];
    pipeline_core dut(.*);
    always #5 clk=~clk;
    always_comb begin
        i_req_ready=(cycle%3 != 0) && iwait<0 && !i_rsp_valid;
        d_req_ready=(cycle%4 == 0) && dwait<0 && !d_rsp_valid;
    end
    always @(posedge clk) begin
        if(rst) begin
            cycle<=0; iwait<=-1; dwait<=-1; i_rsp_valid<=0; d_rsp_valid<=0;
            stores<=0; retired<=0;
        end else begin
            cycle<=cycle+1;
            if(retire_valid) begin
                if(retired>=23 || retire_pc!==expected_pc[retired])
                    $fatal(1,"retirement mismatch index=%0d pc=%h",retired,retire_pc);
                retired<=retired+1;
            end
            if(i_rsp_valid && i_rsp_ready) i_rsp_valid<=0;
            if(iwait>0) iwait<=iwait-1;
            else if(iwait==0) begin i_rsp_valid<=1; iwait<=-1; end
            if(i_req_valid && i_req_ready) begin
                if(i_req_addr[1:0]!=0 || i_req_addr>=56) $fatal(1,"bad instruction address");
                i_rsp_data[31:0]<=rom[i_req_addr>>2];
                i_rsp_data[63:32]<=i_req_addr<52 ? rom[(i_req_addr>>2)+1] : 0;
                i_rsp_error<=0; iwait<=2;
            end
            if(d_rsp_valid && d_rsp_ready) d_rsp_valid<=0;
            if(dwait>0) dwait<=dwait-1;
            else if(dwait==0) begin d_rsp_valid<=1; dwait<=-1; end
            if(d_req_valid && d_req_ready) begin
                if(d_req_addr[1:0]!=0 || d_req_addr>=64) $fatal(1,"bad data address");
                d_rsp_data<=ram[d_req_addr>>2]; d_rsp_error<=0; dwait<=3;
                if(d_req_write) begin
                    for(j=0;j<4;j=j+1)
                        if(d_req_wstrb[j]) ram[d_req_addr>>2][j*8 +: 8]<=d_req_wdata[j*8 +: 8];
                    stores<=stores+1;
                end
            end
        end
    end
    integer idx,k;
    initial begin
        rom[0]=32'h00000093; rom[1]=32'h00500113;
        rom[2]=32'h002080b3; rom[3]=32'hfff10113;
        rom[4]=32'hfe011ce3; rom[5]=32'h00102023;
        rom[6]=32'h00002183; rom[7]=32'h40118233;
        rom[8]=32'h00020463; rom[9]=32'h06300393;
        rom[10]=32'h008002ef; rom[11]=32'h06200393;
        rom[12]=32'h00028313; rom[13]=32'hffffffff;
        for(k=0;k<16;k=k+1) ram[k]=0;
        expected_pc[0]=0; expected_pc[1]=4; idx=2;
        for(k=0;k<5;k=k+1) begin
            expected_pc[idx]=8; expected_pc[idx+1]=12; expected_pc[idx+2]=16; idx=idx+3;
        end
        expected_pc[17]=20; expected_pc[18]=24; expected_pc[19]=28;
        expected_pc[20]=32; expected_pc[21]=40; expected_pc[22]=48;
        repeat(2) @(negedge clk); rst=0;
        wait(fault); repeat(3) @(negedge clk);
        if(retired!==23 || stores!==1 || ram[0]!==15 ||
           dut.regs[1]!==15 || dut.regs[3]!==15 || dut.regs[5]!==44 || dut.regs[6]!==44)
            $fatal(1,"wrong outcome retired=%0d stores=%0d ram=%0d",retired,stores,ram[0]);
        $display("PASS pipeline_core: retired=%0d sum=%0d stores=%0d cycles=%0d",retired,ram[0],stores,cycle);
        $finish;
    end
    initial begin #20000; $fatal(1,"timeout"); end
endmodule
