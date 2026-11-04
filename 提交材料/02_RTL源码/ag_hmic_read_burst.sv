// Collect ordered single-line reads into true HMIC INCR bursts. Addresses use
// 32-bit words, so one 256-bit line advances by eight. Upstream reserves every
// return before its request is accepted; the controller has no RREADY.
module ag_hmic_read_burst #(
 parameter integer DEPTH=32,MAX_BURST=16,COALESCE_CYCLES=3
)(
 input logic clk,reset_n,enable,
 input logic [27:0] in_araddr,input logic in_arvalid,output logic in_arready,
 output logic [27:0] out_araddr,output logic [3:0] out_arlen,
 output logic out_arvalid,input logic out_arready,
 input logic [255:0] out_rdata,input logic out_rvalid,out_rlast,input logic [3:0] out_rid,
 output logic [255:0] in_rdata,output logic in_rvalid,in_rlast,output logic [3:0] in_rid
);
 localparam integer PTR=$clog2(DEPTH),WAIT_W=$clog2(COALESCE_CYCLES+2);
 logic [27:0] addresses[0:DEPTH-1];
 logic [DEPTH-1:0] adjacent;
 logic [PTR-1:0] previous_tail;
 assign previous_tail=tail-1'b1;
 logic expected_last[0:DEPTH-1];
 logic [PTR-1:0] head,tail,response_head,response_tail;
 logic [PTR:0] count,response_count;
 logic [WAIT_W-1:0] age;
 logic command_valid;
 logic [4:0] command_beats;
 logic [4:0] run_length,planned_beats;
 logic [27:0] planned_addr;
 logic plan_valid;
 logic [15:0] break_mask,scan;
 logic [3:0] position;
 integer n;
 wire accept=in_arvalid&&in_arready;
 wire issue=out_arvalid&&out_arready;
 wire consume=enable&&out_rvalid&&(response_count!=0||issue);
 wire expected=response_count!=0?expected_last[response_head]:(issue&&command_beats==1);
 assign in_arready=enable&&count<DEPTH;
 assign out_arvalid=enable&&command_valid;
 assign out_arlen=command_beats-1'b1;
 assign in_rdata=out_rdata;
 assign in_rid=out_rid;
 assign in_rvalid=enable&&out_rvalid;
 // Single-line bridge expects LAST on every response. A malformed downstream
 // LAST becomes a false upstream LAST and follows the bridge's fault drain.
 assign in_rlast=(response_count!=0||issue)&&(out_rlast==expected);
 always_comb begin
  break_mask=0;
  for(integer k=1;k<16;k=k+1)
   break_mask[k]=k>=MAX_BURST||k>=count||!adjacent[(head+k)%DEPTH];
  scan=break_mask;position=0;
  if(scan[7:0]==0)begin scan=scan>>8;position=position+8;end
  if(scan[3:0]==0)begin scan=scan>>4;position=position+4;end
  if(scan[1:0]==0)begin scan=scan>>2;position=position+2;end
  if(!scan[0])position=position+1'b1;
  run_length=break_mask==0?5'd16:{1'b0,position};
 end
 always_ff @(posedge clk or negedge reset_n)begin
  if(!reset_n)begin
   head<=0;tail<=0;response_head<=0;response_tail<=0;count<=0;response_count<=0;
   age<=0;command_valid<=0;command_beats<=1;out_araddr<=0;adjacent<=0;plan_valid<=0;planned_beats<=1;planned_addr<=0;
  end else if(!enable)begin
   head<=0;tail<=0;response_head<=0;response_tail<=0;count<=0;response_count<=0;
   age<=0;command_valid<=0;command_beats<=1;adjacent<=0;plan_valid<=0;
  end else begin
   if(accept)begin
    addresses[tail]<=in_araddr;tail<=tail+1'b1;
    adjacent[tail]<=count!=0&&in_araddr==addresses[previous_tail]+28'd8&&in_araddr[27:10]==addresses[previous_tail][27:10];
   end
   case({accept,issue})
    2'b10:count<=count+1'b1;
    2'b01:count<=count-command_beats;
    2'b11:count<=count+1'b1-command_beats;
    default:begin end
   endcase
   if(count==0||issue)age<=0;
   else if(age<COALESCE_CYCLES)age<=age+1'b1;
   if(!command_valid&&!plan_valid&&count!=0&&(count>=MAX_BURST||age>=COALESCE_CYCLES))begin
    plan_valid<=1;planned_beats<=run_length;planned_addr<=addresses[head];
   end
   if(plan_valid&&response_count+planned_beats<=DEPTH)begin
    plan_valid<=0;command_valid<=1;command_beats<=planned_beats;out_araddr<=planned_addr;
   end
   if(issue)begin
    command_valid<=0;head<=head+command_beats;
    response_tail<=response_tail+command_beats;
    for(n=0;n<MAX_BURST;n=n+1)if(n<command_beats)
     expected_last[(response_tail+n)%DEPTH]<=n==command_beats-1;
   end
   if(consume)response_head<=response_head+1'b1;
   case({issue,consume})
    2'b10:response_count<=response_count+command_beats;
    2'b01:response_count<=response_count-1'b1;
    2'b11:response_count<=response_count+command_beats-1'b1;
    default:begin end
   endcase
  end
 end
 initial begin
  if(DEPTH<4||(DEPTH&(DEPTH-1))!=0||MAX_BURST<1||MAX_BURST>16||MAX_BURST>DEPTH)
   $error("invalid HMIC burst geometry");
 end
endmodule
