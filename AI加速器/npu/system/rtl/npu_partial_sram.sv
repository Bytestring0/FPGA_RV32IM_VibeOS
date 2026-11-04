// Optional64KiB line SRAM in the shared physical address space. All CPU/cache,
// matrix and tensor traffic traverses this router; no incoherent private alias.
// Mapping is startup-only in the supported runtime. Before any manual disable
// or remap, quiesce all masters and clean/invalidate BOTH old and new CPU cache
// spans. Bus idle alone cannot prove upstream cache quiescence.
// DDR requests retain four ordered credits. A local transaction starts only
// after older DDR responses drain, and blocks later DDR until its own ACK.
module npu_partial_sram #(
 parameter logic [31:0] CSR_BASE=32'h10007000,
 parameter integer LINE_BITS=11, parameter integer DDR_DEPTH=4
)(
 input wire clk,rst,
 input wire req_valid,output wire req_ready,input wire req_write,
 input wire [31:0] req_addr,req_wdata,input wire [3:0] req_wstrb,
 output reg rsp_valid,input wire rsp_ready,output reg [31:0] rsp_rdata,output reg rsp_error,
 input wire bus_req_valid,output wire bus_req_ready,input wire [29:0] bus_req_addr,
 input wire bus_req_write,input wire [255:0] bus_req_wdata,input wire [31:0] bus_req_wstrb,
 output wire bus_rsp_valid,input wire bus_rsp_ready,output wire [255:0] bus_rsp_data,output wire bus_rsp_error,
 output wire mem_req_valid,input wire mem_req_ready,output wire [29:0] mem_req_addr,
 output wire mem_req_write,output wire [255:0] mem_req_wdata,output wire [31:0] mem_req_wstrb,
 input wire mem_rsp_valid,output wire mem_rsp_ready,input wire [255:0] mem_rsp_data,input wire mem_rsp_error
);
 localparam integer BYTES=32*(1<<LINE_BITS);
 reg [29:0] base;reg enabled,local_pending,local_error;
 reg [255:0] local_data;
 reg [2:0] ddr_count;
 reg [31:0] local_reads,local_writes;
 // Each lane is a single-port synchronous byte RAM, not a256-bit dynamic
 // partial-write mux. The byte-enable is local to its bank; a full-line reset
 // writes one address per clock into every bank without resetting the array.
 // This costs32 byte-wide banks but leaves their timing and RAM inference clear.
 reg initializing;reg [LINE_BITS-1:0] reset_index;
 wire hit=enabled&&bus_req_addr>=base&&({2'd0,bus_req_addr}-{2'd0,base})<BYTES;
 wire [LINE_BITS-1:0] index=bus_req_addr[LINE_BITS+4:5];
 wire ddr_pop=mem_rsp_valid&&mem_rsp_ready;
 wire local_take=bus_req_valid&&bus_req_ready&&hit;
 wire ddr_take=mem_req_valid&&mem_req_ready;
 wire local_access_error=bus_req_addr[4:0]!=0;
 wire idle=ddr_count==0&&!local_pending;
 assign req_ready=!rst&&!rsp_valid;
 assign mem_req_valid=!rst&&!initializing&&!local_pending&&!hit&&bus_req_valid&&ddr_count<DDR_DEPTH;
 assign bus_req_ready=!rst&&!initializing&&!local_pending&&(hit?ddr_count==0:(ddr_count<DDR_DEPTH&&mem_req_ready));
 assign mem_req_addr=bus_req_addr;assign mem_req_write=bus_req_write;
 assign mem_req_wdata=bus_req_wdata;assign mem_req_wstrb=bus_req_wstrb;
 assign bus_rsp_valid=!rst&&(local_pending||(ddr_count!=0&&mem_rsp_valid));
 assign bus_rsp_data=local_pending?local_data:mem_rsp_data;
 assign bus_rsp_error=local_pending?local_error:mem_rsp_error;
 assign mem_rsp_ready=!rst&&!local_pending&&ddr_count!=0&&bus_rsp_ready;
 genvar half_lane;
 generate for(half_lane=0;half_lane<16;half_lane=half_lane+1)begin: sram_halves
  // 2048x16 bits matches one36K DRM per bank; native byte write-enables.
  reg [15:0] half_ram[0:(1<<LINE_BITS)-1] /* synthesis syn_ramstyle = "block_ram" */;
  wire [LINE_BITS-1:0] ram_addr=initializing?reset_index:index;
  wire [15:0] ram_data=initializing?16'd0:bus_req_wdata[half_lane*16+:16];
  wire [1:0] enable=initializing?2'b11:
    {2{local_take&&bus_req_write&&!local_access_error}}&bus_req_wstrb[half_lane*2+:2];
  always @(posedge clk)begin
   if(!rst)begin
    if(enable[0])half_ram[ram_addr][7:0]<=ram_data[7:0];
    if(enable[1])half_ram[ram_addr][15:8]<=ram_data[15:8];
   end
   if(local_take&&!bus_req_write)local_data[half_lane*16+:16]<=half_ram[ram_addr];
  end
 end endgenerate
 always @(posedge clk)begin
  if(rst)begin
   base<=0;enabled<=0;ddr_count<=0;local_pending<=0;local_error<=0;
   rsp_valid<=0;rsp_rdata<=0;rsp_error<=0;local_reads<=0;local_writes<=0;
   initializing<=1;reset_index<=0;
  end else begin
   if(initializing)begin if(&reset_index)initializing<=0;else reset_index<=reset_index+1'b1;end
   if(rsp_valid&&rsp_ready)rsp_valid<=0;
   if(local_pending&&bus_rsp_ready)local_pending<=0;
   if(local_take)begin
    local_pending<=1;local_error<=local_access_error;
    if(bus_req_write)begin
     local_writes<=local_writes+1'b1;
    end else local_reads<=local_reads+1'b1;
   end
   case({ddr_take,ddr_pop})2'b10:ddr_count<=ddr_count+1'b1;2'b01:ddr_count<=ddr_count-1'b1;default:begin end endcase
   if(req_valid&&req_ready)begin
    rsp_valid<=1;rsp_error<=0;rsp_rdata<=0;
    if(req_addr[31:12]!=CSR_BASE[31:12]||req_addr[1:0]!=0)rsp_error<=1;
    else if(req_write)begin
     // Freeze the mapping while any bus request is offered, including a held one.
     if(req_wstrb!=15||!idle||initializing||bus_req_valid)rsp_error<=1;
     else case(req_addr[11:0])
      0:if(req_wdata==0)enabled<=0;
        else if(req_wdata==1&&!enabled)begin enabled<=1;initializing<=1;reset_index<=0;end
        else rsp_error<=1;
      4:if(enabled||req_wdata[31:30]!=0||req_wdata[LINE_BITS+4:0]!=0)rsp_error<=1;else base<=req_wdata[29:0];
      default:rsp_error<=1;
     endcase
    end else case(req_addr[11:0])
     0:rsp_rdata<={30'd0,initializing,enabled};4:rsp_rdata<={2'd0,base};
     8:rsp_rdata<=32'h50535231;12:rsp_rdata<=BYTES;
     16:rsp_rdata<=local_reads;20:rsp_rdata<=local_writes;
     default:rsp_error<=1;
    endcase
   end
  end
 end
endmodule
