// Host schedules the graph; both numerical engines use production RTL.
module model_npu_rtl #(parameter integer WEIGHT_DEPTH=4)(
 input wire clk,rst,select_tensor,input wire req_valid,output wire req_ready,
 input wire req_write,input wire [31:0] req_addr,req_wdata,input wire [3:0] req_wstrb,
 output wire rsp_valid,input wire rsp_ready,output wire [31:0] rsp_rdata,output wire rsp_error,
 output wire mem_req_valid,input wire mem_req_ready,output wire [29:0] mem_req_addr,
 output wire mem_req_write,output wire [255:0] mem_req_wdata,output wire [31:0] mem_req_wstrb,
 input wire mem_rsp_valid,output wire mem_rsp_ready,input wire [255:0] mem_rsp_data,input wire mem_rsp_error);
 wire [1:0] qr,rv,re,mv,mw,mr;wire [31:0] rd[0:1],ms[0:1];wire [29:0] ma[0:1];wire [255:0] md[0:1];
 npu_dma_top #(.LANES(64),.WEIGHT_DEPTH(WEIGHT_DEPTH)) matrix(.clk(clk),.rst(rst),.req_valid(req_valid&&!select_tensor),.req_ready(qr[0]),
 .req_write(req_write),.req_addr(req_addr),.req_wdata(req_wdata),.req_wstrb(req_wstrb),
 .rsp_valid(rv[0]),.rsp_ready(rsp_ready&&!select_tensor),.rsp_rdata(rd[0]),.rsp_error(re[0]),
 .mem_req_valid(mv[0]),.mem_req_ready(mem_req_ready&&!select_tensor),.mem_req_addr(ma[0]),.mem_req_write(mw[0]),
 .mem_req_wdata(md[0]),.mem_req_wstrb(ms[0]),.mem_rsp_valid(mem_rsp_valid&&!select_tensor),.mem_rsp_ready(mr[0]),
 .mem_rsp_data(mem_rsp_data),.mem_rsp_error(mem_rsp_error),.busy(),.done(),.error(),.error_code(),.cycle_count(),.result_count());
 npu_tensor_v2 #(.PARALLEL_RAW(0),.SHARED_RAW_ACCUM(1),.DELTA_PREFETCH(WEIGHT_DEPTH)) tensor(.clk(clk),.rst(rst),.req_valid(req_valid&&select_tensor),.req_ready(qr[1]),
 .req_write(req_write),.req_addr(req_addr),.req_wdata(req_wdata),.req_wstrb(req_wstrb),
 .rsp_valid(rv[1]),.rsp_ready(rsp_ready&&select_tensor),.rsp_rdata(rd[1]),.rsp_error(re[1]),
 .mem_req_valid(mv[1]),.mem_req_ready(mem_req_ready&&select_tensor),.mem_req_addr(ma[1]),.mem_req_write(mw[1]),
 .mem_req_wdata(md[1]),.mem_req_wstrb(ms[1]),.mem_rsp_valid(mem_rsp_valid&&select_tensor),.mem_rsp_ready(mr[1]),
 .mem_rsp_data(mem_rsp_data),.mem_rsp_error(mem_rsp_error),.busy(),.done(),.error(),.error_code(),.cycle_count(),.result_count());
 assign req_ready=qr[select_tensor];assign rsp_valid=rv[select_tensor];assign rsp_rdata=rd[select_tensor];assign rsp_error=re[select_tensor];
 assign mem_req_valid=mv[select_tensor];assign mem_req_write=mw[select_tensor];assign mem_req_addr=ma[select_tensor];
 assign mem_req_wdata=md[select_tensor];assign mem_req_wstrb=ms[select_tensor];assign mem_rsp_ready=mr[select_tensor];
endmodule
