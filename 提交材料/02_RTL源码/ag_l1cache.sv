// Two independent 2-way caches. Synchronous line/tag reads infer block RAM.
// One external 32-byte transaction at a time; D is write-back/write-allocate.
module ag_l1cache #(parameter integer SET_BITS=7, parameter bit FAST_RANGE_SCAN=1) (
 input logic clk,rst, maintenance_req,
 input logic maintenance_range,
 input logic [31:0] maintenance_start,maintenance_end,
 output logic maintenance_busy,maintenance_error,
 input logic req_valid, output logic req_ready,
 input logic req_instruction,req_write,
 input logic [29:0] req_addr,
 input logic [255:0] req_wdata,input logic [31:0] req_wstrb,
 output logic rsp_valid,input logic rsp_ready,
 output logic [255:0] rsp_data,output logic rsp_error,
 output logic mem_req_valid,input logic mem_req_ready,
 output logic [29:0] mem_req_addr,output logic mem_req_write,
 output logic [255:0] mem_req_wdata,output logic [31:0] mem_req_wstrb,
 input logic mem_rsp_valid,output logic mem_rsp_ready,
 input logic [255:0] mem_rsp_data,input logic mem_rsp_error,
 output logic [31:0] i_hits,i_misses,d_hits,d_misses,writebacks
);
 localparam integer SETS=1<<SET_BITS, TAG_BITS=25-SET_BITS;
 localparam [3:0] IDLE=0,LOOKUP=1,WB_SEND=2,WB_WAIT=3,FILL_SEND=4,FILL_WAIT=5,SCAN_READ=6,SCAN_CHECK=7,RESET_SCAN=8;
 logic [3:0] state;
 logic [255:0] i_data0[0:SETS-1],i_data1[0:SETS-1],d_data0[0:SETS-1],d_data1[0:SETS-1];
 logic [TAG_BITS-1:0] i_tag0[0:SETS-1],i_tag1[0:SETS-1],d_tag0[0:SETS-1],d_tag1[0:SETS-1];
 // Small synchronous metadata RAMs replace resettable bitmaps and their
 // variable-index write decoders. Reset walks the sets instead of resetting
 // every bit, allowing RAM inference. Layout: I={LRU,V1,V0}, D={LRU,D1,D0,V1,V0}.
 logic [2:0] i_meta[0:SETS-1],imr;
 logic [4:0] d_meta[0:SETS-1],dmr;
 logic [255:0] ir0,ir1,dr0,dr1,evict_data;
 logic [TAG_BITS-1:0] it0,it1,dt0,dt1;
 logic instruction,writing,victim,flushing,flush_pending;
 logic pending_range,active_range;
 logic [31:0] pending_start,pending_end,active_start,active_end;
 logic store_commit,store_way;
 wire [255:0] rd0=(instruction && !flushing)?ir0:dr0, rd1=(instruction && !flushing)?ir1:dr1;
 wire [TAG_BITS-1:0] rt0=(instruction && !flushing)?it0:dt0, rt1=(instruction && !flushing)?it1:dt1;
 logic [29:0] address,evict_addr;
 logic [255:0] write_data;
 logic [31:0] write_mask;
 logic [SET_BITS-1:0] scan,scan_stop;
 wire [31:0] pending_last_byte=pending_end-32'd1;
 wire [31:0] pending_lines=(pending_last_byte>>5)-(pending_start>>5)+32'd1;
 wire short_range=FAST_RANGE_SCAN&&pending_range&&pending_end>pending_start&&pending_lines<SETS&&
                  !(maintenance_req&&!maintenance_range);
 // Small ranges scan only the affected set indices, wrapping modulo SETS.
 // Reconstruct complete physical tags for both ways; aliases outside the range
 // remain valid/dirty. Ranges covering every set and full FENCE.I scan all sets.
 wire [31:0] scan_d0={2'b00,dt0,scan,5'b0},scan_d1={2'b00,dt1,scan,5'b0};
 wire [31:0] scan_i0={2'b00,it0,scan,5'b0},scan_i1={2'b00,it1,scan,5'b0};
 wire match_d0=!active_range||(scan_d0>=active_start&&scan_d0<active_end);
 wire match_d1=!active_range||(scan_d1>=active_start&&scan_d1<active_end);
 wire match_i0=!active_range||(scan_i0>=active_start&&scan_i0<active_end);
 wire match_i1=!active_range||(scan_i1>=active_start&&scan_i1<active_end);
 wire dirty0=dmr[0]&&dmr[2]&&match_d0,dirty1=dmr[1]&&dmr[3]&&match_d1;
 wire [SET_BITS-1:0] index=address[5+:SET_BITS];
 wire [TAG_BITS-1:0] tag=address[29:5+SET_BITS];
 wire valid0=(instruction && !flushing)?imr[0]:dmr[0];
 wire valid1=(instruction && !flushing)?imr[1]:dmr[1];
 wire read_lru=instruction?imr[2]:dmr[4];
 wire hit0=valid0 && rt0==tag, hit1=valid1 && rt1==tag;
 // Used only on a miss; hit-way selection stays independent of replacement.
 wire chosen=!valid0?1'b0:!valid1?1'b1:read_lru;
 function automatic [255:0] merge_bytes(input logic [255:0] old_data,new_data,input logic [31:0] mask);
  integer b;
  begin
   for(b=0;b<32;b=b+1) merge_bytes[8*b+:8]=mask[b]?new_data[8*b+:8]:old_data[8*b+:8];
  end
 endfunction
 assign maintenance_busy=flush_pending || flushing || state==RESET_SCAN;
 assign req_ready=!rst && state==IDLE && !rsp_valid && !flush_pending;
 assign mem_req_valid=!rst && (state==WB_SEND || state==FILL_SEND);
 assign mem_req_write=state==WB_SEND;
 assign mem_req_addr=state==WB_SEND?evict_addr:address;
 assign mem_req_wdata=evict_data;
 assign mem_req_wstrb=state==WB_SEND?32'hffffffff:32'b0;
 assign mem_rsp_ready=!rst && (state==WB_WAIT || state==FILL_WAIT);
 wire accept=req_valid && req_ready;
 wire d_read=(accept && !req_instruction) || state==SCAN_READ;
 wire [SET_BITS-1:0] d_read_index=state==SCAN_READ?scan:req_addr[5+:SET_BITS];
 // A single read address per D array prevents PDS from replicating RAMs for
 // demand lookup and maintenance. No reset of any line/tag/metadata array.
 always_ff @(posedge clk) begin
  if((accept && req_instruction)||state==SCAN_READ) begin
    imr<=i_meta[d_read_index];
    ir0<=i_data0[d_read_index];ir1<=i_data1[d_read_index];
    it0<=i_tag0[d_read_index];it1<=i_tag1[d_read_index];
  end
  if(d_read) begin
   dmr<=d_meta[d_read_index];
   dr0<=d_data0[d_read_index];dr1<=d_data1[d_read_index];
   dt0<=d_tag0[d_read_index];dt1<=d_tag1[d_read_index];
  end else if(state==WB_WAIT && mem_rsp_valid && !mem_rsp_error) begin
   // Recheck the other way during maintenance without rereading the data.
   if(victim) dmr[3]<=0;else dmr[2]<=0;
  end
 end
 // Retire a hit store into the existing response-hold cycle. No next request
 // can be accepted while rsp_valid is set, so the saved index/data remain
 // stable. Tag compare no longer drives the synchronous data-RAM write port.
 always_ff @(posedge clk) begin
  if(!rst) begin
   if(store_commit) begin
    if(store_way) d_data1[index]<=merge_bytes(dr1,write_data,write_mask);
    else d_data0[index]<=merge_bytes(dr0,write_data,write_mask);
   end else if(state==FILL_WAIT && mem_rsp_valid && !mem_rsp_error && !instruction) begin
    if(victim) d_data1[index]<=writing?merge_bytes(mem_rsp_data,write_data,write_mask):mem_rsp_data;
    else d_data0[index]<=writing?merge_bytes(mem_rsp_data,write_data,write_mask):mem_rsp_data;
   end
  end
 end
 always_ff @(posedge clk) begin
  if(rst) begin
   state<=RESET_SCAN;
   store_commit<=0;store_way<=0;
   flush_pending<=0;flushing<=0;maintenance_error<=0;scan<=0;scan_stop<=0;
   pending_range<=0;active_range<=0;
   pending_start<=0;pending_end<=0;active_start<=0;active_end<=0;
   rsp_valid<=0;rsp_data<=0;rsp_error<=0;
   address<=0;instruction<=0;writing<=0;write_data<=0;write_mask<=0;
   evict_addr<=0;evict_data<=0;victim<=0;
   i_hits<=0;i_misses<=0;d_hits<=0;d_misses<=0;writebacks<=0;
  end else begin
   store_commit<=0;
   if(rsp_ready) rsp_valid<=0;
   if(maintenance_req) begin
    if(!flush_pending&&!flushing&&state!=RESET_SCAN) begin
     flush_pending<=1;pending_range<=maintenance_range;
     pending_start<=maintenance_start;pending_end<=maintenance_end;
    end else if(!maintenance_range) begin
     // Queue a full FENCE.I without changing the active range.
     flush_pending<=1;pending_range<=0;
    end
   end
   case(state)
    RESET_SCAN: begin
     i_meta[scan]<=0;d_meta[scan]<=0;
     if(&scan) begin scan<=0;state<=IDLE;end
     else scan<=scan+1'b1;
    end
    IDLE: begin
     if(flush_pending && !rsp_valid) begin
      flushing<=1;flush_pending<=0;state<=SCAN_READ;
      scan<=short_range?pending_start[5+:SET_BITS]:{SET_BITS{1'b0}};
      scan_stop<=short_range?pending_last_byte[5+:SET_BITS]:{SET_BITS{1'b1}};
      active_range<=pending_range && !(maintenance_req && !maintenance_range);
      active_start<=pending_start;active_end<=pending_end;
     end else if(req_valid && req_ready) begin
      address<=req_addr;instruction<=req_instruction;writing<=req_write;
      write_data<=req_wdata;write_mask<=req_wstrb;
      rsp_error<=0;
      if(maintenance_error || req_addr[4:0]!=0 || (req_instruction && req_write)) begin
       rsp_valid<=1;rsp_error<=1;rsp_data<=0;
      end else state<=LOOKUP;
     end
    end
    LOOKUP: begin
     if(hit0 || hit1) begin
      if(instruction) begin
       i_hits<=i_hits+1'b1;i_meta[index]<={hit0,imr[1:0]};
      end else begin
       d_hits<=d_hits+1'b1;
       d_meta[index]<={hit0,dmr[3] || (writing && !hit0),dmr[2] || (writing && hit0),dmr[1:0]};
       if(writing) begin
        store_commit<=1;store_way<=!hit0;
       end
      end
      rsp_data<=writing?merge_bytes(hit0?rd0:rd1,write_data,write_mask):(hit0?rd0:rd1);
      rsp_valid<=1;rsp_error<=0;state<=IDLE;
     end else begin
      if(instruction) i_misses<=i_misses+1'b1;else d_misses<=d_misses+1'b1;
      victim<=chosen;evict_addr<={chosen?rt1:rt0,index,5'b0};evict_data<=chosen?rd1:rd0;
      if(!instruction && (chosen?(valid1&&dmr[3]):(valid0&&dmr[2]))) state<=WB_SEND;
      else state<=FILL_SEND;
     end
    end
    WB_SEND: if(mem_req_ready) state<=WB_WAIT;
    WB_WAIT: if(mem_rsp_valid) begin
     if(mem_rsp_error) begin
      if(flushing) begin maintenance_error<=1;flushing<=0;end
      else begin rsp_error<=1;rsp_data<=0;rsp_valid<=1;end
      // Preserve dirty victim on failed writeback, never install the new line.
      state<=IDLE;
     end else begin
      writebacks<=writebacks+1'b1;
      if(flushing) begin
       d_meta[scan]<=victim?{dmr[4],1'b0,dmr[2:0]}:{dmr[4:3],1'b0,dmr[1:0]};
       state<=SCAN_CHECK;
      end else begin
       d_meta[index]<=victim?{dmr[4],1'b0,dmr[2:0]}:{dmr[4:3],1'b0,dmr[1:0]};
       state<=FILL_SEND;
      end
     end
    end
    FILL_SEND: if(mem_req_ready) state<=FILL_WAIT;
    FILL_WAIT: if(mem_rsp_valid) begin
     rsp_valid<=1;rsp_error<=mem_rsp_error;
     rsp_data<=mem_rsp_error?256'b0:(writing?merge_bytes(mem_rsp_data,write_data,write_mask):mem_rsp_data);
     if(!mem_rsp_error) begin
      if(instruction) begin
       i_meta[index]<={!victim,victim?1'b1:valid1,victim?valid0:1'b1};
       if(victim) begin i_data1[index]<=mem_rsp_data;i_tag1[index]<=tag;end
       else begin i_data0[index]<=mem_rsp_data;i_tag0[index]<=tag;end
      end else begin
       d_meta[index]<={!victim,victim?writing:dmr[3],victim?dmr[2]:writing,victim?1'b1:valid1,victim?valid0:1'b1};
       if(victim) d_tag1[index]<=tag;
       else d_tag0[index]<=tag;
      end
     end
     state<=IDLE;
    end
    SCAN_READ: state<=SCAN_CHECK;
    SCAN_CHECK: begin
     if(dirty0 || dirty1) begin
      victim<=!dirty0;
      evict_addr<={dirty0?dt0:dt1,scan,5'b0};
      evict_data<=dirty0?dr0:dr1;
      state<=WB_SEND;
     end else begin
      if(!active_range) begin i_meta[scan]<=0;d_meta[scan]<=0;end
      else begin
       i_meta[scan]<={imr[2],imr[1]&&!match_i1,imr[0]&&!match_i0};
       d_meta[scan]<={dmr[4],dmr[3]&&!match_d1,dmr[2]&&!match_d0,
                     dmr[1]&&!match_d1,dmr[0]&&!match_d0};
      end
      if(scan==scan_stop) begin flushing<=0;state<=IDLE;end
      else begin scan<=scan+1'b1;state<=SCAN_READ;end
     end
    end
    default:state<=IDLE;
   endcase
  end
 end
endmodule
