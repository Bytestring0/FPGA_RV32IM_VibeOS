// Five active-low board keys. Synchronize and debounce each pin independently.
// Events accumulate until the accepted EVENTS read snapshots and clears them.
module ag_button_top #(
 parameter integer CLOCK_HZ=102000000,
 parameter integer DEBOUNCE_CYCLES=CLOCK_HZ/100,
 parameter integer LONG_CYCLES=CLOCK_HZ,
 parameter [31:0] CSR_BASE=32'h10001210
)(
 input wire clk,rst,input wire [4:0] buttons_n,
 input wire req_valid,output wire req_ready,input wire req_write,
 input wire [31:0] req_addr,req_wdata,input wire [3:0] req_wstrb,
 output reg rsp_valid,input wire rsp_ready,output reg [31:0] rsp_rdata,output reg rsp_error
);
 localparam integer DB_BITS=DEBOUNCE_CYCLES>1?$clog2(DEBOUNCE_CYCLES+1):1;
 localparam integer LONG_BITS=LONG_CYCLES>1?$clog2(LONG_CYCLES+1):1;
 (* async_reg="true" *) reg [4:0] pin_meta,pin_sync;
 reg [4:0] level,long_sent,press_pulse,release_pulse,long_pulse;
 reg [DB_BITS-1:0] debounce[0:4];reg [LONG_BITS-1:0] held[0:4];
 reg [4:0] press_events,release_events,long_events;
 wire accept=req_valid&&req_ready;
 wire clear_events=accept&&!req_write&&req_addr==CSR_BASE;
 assign req_ready=!rst&&!rsp_valid;
 integer key;
 always @(posedge clk)begin
  if(rst)begin
   pin_meta<=5'b11111;pin_sync<=5'b11111;level<=0;long_sent<=0;
   press_pulse<=0;release_pulse<=0;long_pulse<=0;
   for(key=0;key<5;key=key+1)begin debounce[key]<=0;held[key]<=0;end
  end else begin
   pin_meta<=buttons_n;pin_sync<=pin_meta;
   press_pulse<=0;release_pulse<=0;long_pulse<=0;
   for(key=0;key<5;key=key+1)begin
    if((!pin_sync[key])==level[key])debounce[key]<=0;
    else if(debounce[key]>=DEBOUNCE_CYCLES-1)begin
     debounce[key]<=0;level[key]<=!pin_sync[key];held[key]<=0;long_sent[key]<=0;
     if(!pin_sync[key])press_pulse[key]<=1;else release_pulse[key]<=1;
    end else debounce[key]<=debounce[key]+1'b1;
    if(!level[key])begin held[key]<=0;long_sent[key]<=0;end
    else if(!long_sent[key])begin
     if(held[key]>=LONG_CYCLES-1)begin long_pulse[key]<=1;long_sent[key]<=1;end
     else held[key]<=held[key]+1'b1;
    end
   end
  end
 end
 always @(posedge clk)begin
  if(rst)begin press_events<=0;release_events<=0;long_events<=0;rsp_valid<=0;rsp_rdata<=0;rsp_error<=0;end
  else begin
   press_events<=(clear_events?5'd0:press_events)|press_pulse;
   release_events<=(clear_events?5'd0:release_events)|release_pulse;
   long_events<=(clear_events?5'd0:long_events)|long_pulse;
   if(rsp_valid&&rsp_ready)rsp_valid<=0;
   if(accept)begin
    rsp_valid<=1;rsp_rdata<=0;rsp_error<=req_write||req_addr[1:0]!=0;
    case(req_addr)
     CSR_BASE:rsp_rdata<={12'd0,level,long_events,release_events,press_events};
     CSR_BASE+4:rsp_rdata<={27'd0,level};
     CSR_BASE+8:rsp_rdata<=32'h4b455935;
     default:rsp_error<=1;
    endcase
   end
  end
 end
endmodule
