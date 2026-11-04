// UART8N1 receive only. Small FIFO permits firmware to consume a packet while
// DDR stores stall; sender waits for a per-packet ACK before sending more.
module ag_uart_rx_top #(
 parameter integer CLOCK_HZ=102000000,
 parameter integer BAUD=115200,
 parameter [31:0] CSR_BASE=32'h10001240
)(
 input wire clk,rst,rx_i,
 input wire req_valid,output wire req_ready,input wire req_write,
 input wire [31:0] req_addr,req_wdata,input wire [3:0] req_wstrb,
 output reg rsp_valid,input wire rsp_ready,output reg [31:0] rsp_rdata,output reg rsp_error
);
 localparam integer DIV=(CLOCK_HZ+BAUD/2)/BAUD;
 localparam integer TIMER_BITS=DIV>1?$clog2(DIV+1):1;
 localparam [1:0] IDLE=0,START=1,DATA=2,STOP=3;
 (* async_reg="true" *)reg rx_meta,rx_sync;
 reg [1:0] state;reg [TIMER_BITS-1:0] timer;
 reg [2:0] bit_index;reg [7:0] shift,received;reg byte_pulse,frame_pulse;
 reg [7:0] fifo[0:31] /* synthesis syn_ramstyle = "select_ram" */;
 reg [4:0] head,tail;reg [5:0] count;reg overflow,framing;
 wire accept=req_valid&&req_ready;
 wire pop=accept&&!req_write&&req_addr==CSR_BASE&&count!=0;
 wire push=byte_pulse&&(count<32||pop);
 assign req_ready=!rst&&!rsp_valid;
 always @(posedge clk)begin
  if(rst)begin rx_meta<=1;rx_sync<=1;state<=IDLE;timer<=0;bit_index<=0;shift<=0;received<=0;byte_pulse<=0;frame_pulse<=0;end
  else begin
   rx_meta<=rx_i;rx_sync<=rx_meta;byte_pulse<=0;frame_pulse<=0;
   case(state)
    IDLE:if(!rx_sync)begin state<=START;timer<=TIMER_BITS'(DIV/2-1);end
    START:if(timer!=0)timer<=timer-1'b1;else if(!rx_sync)begin state<=DATA;timer<=TIMER_BITS'(DIV-1);bit_index<=0;end else state<=IDLE;
    DATA:if(timer!=0)timer<=timer-1'b1;else begin
     shift[bit_index]<=rx_sync;timer<=TIMER_BITS'(DIV-1);
     if(bit_index==7)state<=STOP;else bit_index<=bit_index+1'b1;
    end
    STOP:if(timer!=0)timer<=timer-1'b1;else begin
     state<=IDLE;if(rx_sync)begin received<=shift;byte_pulse<=1;end else frame_pulse<=1;
    end
   endcase
  end
 end
 always @(posedge clk)begin
  if(rst)begin head<=0;tail<=0;count<=0;overflow<=0;framing<=0;rsp_valid<=0;rsp_rdata<=0;rsp_error<=0;end
  else begin
   if(push)begin fifo[tail]<=received;tail<=tail+5'd1;end
   if(pop)head<=head+5'd1;
   case({push,pop})2'b10:count<=count+6'd1;2'b01:count<=count-6'd1;default:begin end endcase
   if(accept&&req_write&&req_addr==CSR_BASE+4&&req_wstrb[0])begin if(req_wdata[1])overflow<=0;if(req_wdata[2])framing<=0;end
   if(byte_pulse&&!push)overflow<=1;if(frame_pulse)framing<=1;
   if(rsp_valid&&rsp_ready)rsp_valid<=0;
   if(accept)begin
    rsp_valid<=1;rsp_rdata<=0;rsp_error<=req_addr[1:0]!=0;
    case(req_addr)
     CSR_BASE:begin rsp_rdata<={24'd0,fifo[head]};if(req_write||count==0)rsp_error<=1;end
     CSR_BASE+4:begin rsp_rdata<={18'd0,count,5'd0,framing,overflow,count!=0};if(req_write&&!req_wstrb[0])rsp_error<=1;end
     CSR_BASE+8:begin rsp_rdata<=32'h55525831;if(req_write)rsp_error<=1;end
     default:rsp_error<=1;
    endcase
   end
  end
 end
endmodule
