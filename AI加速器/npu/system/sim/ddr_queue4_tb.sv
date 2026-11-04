module ddr_queue4_tb #(
 parameter integer DEPTH=4,parameter integer TIMEOUT_BITS=10,MAX_READ_BURST=1
)(
 input logic cpu_clk,ui_clk,reset_n,init_done,
 output wire ready_cpu,fault_cpu,
 input wire a_req_valid,output wire a_req_ready,input wire [29:0] a_req_addr,input wire a_req_write,
 input wire [255:0] a_req_wdata,input wire [31:0] a_req_wstrb,
 output wire a_rsp_valid,input wire a_rsp_ready,output wire [255:0] a_rsp_data,output wire a_rsp_error,
 input wire b_req_valid,output wire b_req_ready,input wire [29:0] b_req_addr,input wire b_req_write,
 input wire [255:0] b_req_wdata,input wire [31:0] b_req_wstrb,
 output wire b_rsp_valid,input wire b_rsp_ready,output wire [255:0] b_rsp_data,output wire b_rsp_error,
 input wire c_req_valid,output wire c_req_ready,input wire [29:0] c_req_addr,input wire c_req_write,
 input wire [255:0] c_req_wdata,input wire [31:0] c_req_wstrb,
 output wire c_rsp_valid,input wire c_rsp_ready,output wire [255:0] c_rsp_data,output wire c_rsp_error,
 output wire [27:0] axi_awaddr,axi_araddr,output wire [3:0] axi_arlen,output wire axi_awvalid,axi_arvalid,
 input wire axi_awready,axi_arready,output wire [255:0] axi_wdata,output wire [31:0] axi_wstrb,
 input wire axi_wready,axi_wusero_last,input wire [3:0] axi_wusero_id,
 input wire [255:0] axi_rdata,input wire axi_rvalid,axi_rlast,input wire [3:0] axi_rid,
 output wire arb_active,arb_owner,
 output wire debug_qv,debug_qr,debug_rv,debug_rr
);
 wire qv,qr,qw,rv,rr,re;
 wire [29:0] qa;wire [255:0] qd,rd;wire [31:0] qs;
 assign debug_qv=qv;assign debug_qr=qr;assign debug_rv=rv;assign debug_rr=rr;
 wire iqv,iqr,iqw,irv,irr,ire,iactive,iowner;
 wire [29:0] iqa;wire [255:0] iqd,ird;wire [31:0] iqs;
 ddr_arbiter2 #(.DEPTH(DEPTH)) accelerator_arb(
 .clk(cpu_clk),.rst(!reset_n),
 .a_req_valid(b_req_valid),.a_req_ready(b_req_ready),.a_req_addr(b_req_addr),.a_req_write(b_req_write),.a_req_wdata(b_req_wdata),.a_req_wstrb(b_req_wstrb),
 .a_rsp_valid(b_rsp_valid),.a_rsp_ready(b_rsp_ready),.a_rsp_data(b_rsp_data),.a_rsp_error(b_rsp_error),
 .b_req_valid(c_req_valid),.b_req_ready(c_req_ready),.b_req_addr(c_req_addr),.b_req_write(c_req_write),.b_req_wdata(c_req_wdata),.b_req_wstrb(c_req_wstrb),
 .b_rsp_valid(c_rsp_valid),.b_rsp_ready(c_rsp_ready),.b_rsp_data(c_rsp_data),.b_rsp_error(c_rsp_error),
 .mem_req_valid(iqv),.mem_req_ready(iqr),.mem_req_addr(iqa),.mem_req_write(iqw),.mem_req_wdata(iqd),.mem_req_wstrb(iqs),
 .mem_rsp_valid(irv),.mem_rsp_ready(irr),.mem_rsp_data(ird),.mem_rsp_error(ire),.active(iactive),.owner(iowner));
 ddr_arbiter2 #(.DEPTH(DEPTH)) arb(
 .clk(cpu_clk),.rst(!reset_n),
 .a_req_valid(a_req_valid),.a_req_ready(a_req_ready),.a_req_addr(a_req_addr),.a_req_write(a_req_write),.a_req_wdata(a_req_wdata),.a_req_wstrb(a_req_wstrb),
 .a_rsp_valid(a_rsp_valid),.a_rsp_ready(a_rsp_ready),.a_rsp_data(a_rsp_data),.a_rsp_error(a_rsp_error),
 .b_req_valid(iqv),.b_req_ready(iqr),.b_req_addr(iqa),.b_req_write(iqw),.b_req_wdata(iqd),.b_req_wstrb(iqs),
 .b_rsp_valid(irv),.b_rsp_ready(irr),.b_rsp_data(ird),.b_rsp_error(ire),
 .mem_req_valid(qv),.mem_req_ready(qr),.mem_req_addr(qa),.mem_req_write(qw),.mem_req_wdata(qd),.mem_req_wstrb(qs),
 .mem_rsp_valid(rv),.mem_rsp_ready(rr),.mem_rsp_data(rd),.mem_rsp_error(re),.active(arb_active),.owner(arb_owner));
 ag_hmic_bridge #(.TIMEOUT_BITS(TIMEOUT_BITS),.DEPTH(DEPTH),.MAX_READ_BURST(MAX_READ_BURST)) bridge(
 .cpu_clk(cpu_clk),.ui_clk(ui_clk),.reset_n(reset_n),.init_done(init_done),.ready_cpu(ready_cpu),.fault_cpu(fault_cpu),
 .req_valid(qv),.req_ready(qr),.req_addr(qa),.req_write(qw),.req_wdata(qd),.req_wstrb(qs),
 .rsp_valid(rv),.rsp_ready(rr),.rsp_data(rd),.rsp_error(re),
 .axi_awaddr(axi_awaddr),.axi_araddr(axi_araddr),.axi_arlen(axi_arlen),.axi_awvalid(axi_awvalid),.axi_arvalid(axi_arvalid),.axi_awready(axi_awready),.axi_arready(axi_arready),
 .axi_wdata(axi_wdata),.axi_wstrb(axi_wstrb),.axi_wready(axi_wready),.axi_wusero_last(axi_wusero_last),.axi_wusero_id(axi_wusero_id),
 .axi_rdata(axi_rdata),.axi_rvalid(axi_rvalid),.axi_rlast(axi_rlast),.axi_rid(axi_rid));
endmodule
