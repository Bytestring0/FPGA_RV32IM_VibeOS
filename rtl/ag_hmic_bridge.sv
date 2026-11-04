// Default DEPTH1 retains the original bridge circuit. DEPTH4 is opt-in.
module ag_hmic_bridge #(
    parameter integer TIMEOUT_BITS = 20,
    parameter integer DEPTH = 1,
    parameter integer MAX_READ_BURST = 1
) (
    input logic cpu_clk, ui_clk, reset_n,
    input logic init_done,
    output logic ready_cpu, fault_cpu,
    input logic req_valid, output logic req_ready,
    input logic [29:0] req_addr, input logic req_write,
    input logic [255:0] req_wdata, input logic [31:0] req_wstrb,
    output logic rsp_valid, input logic rsp_ready,
    output logic [255:0] rsp_data, output logic rsp_error,
    output logic [27:0] axi_awaddr, axi_araddr,
    output logic [3:0] axi_arlen,
    output logic axi_awvalid, axi_arvalid,
    input logic axi_awready, axi_arready,
    output logic [255:0] axi_wdata, output logic [31:0] axi_wstrb,
    input logic axi_wready, axi_wusero_last, input logic [3:0] axi_wusero_id,
    input logic [255:0] axi_rdata, input logic axi_rvalid, axi_rlast,
    input logic [3:0] axi_rid
);
    generate if(DEPTH==1)begin: single
        assign axi_arlen=0;
        ag_hmic_bridge_single #(.TIMEOUT_BITS(TIMEOUT_BITS)) impl(
            .cpu_clk(cpu_clk),.ui_clk(ui_clk),.reset_n(reset_n),.init_done(init_done),
            .ready_cpu(ready_cpu),.fault_cpu(fault_cpu),
            .req_valid(req_valid),.req_ready(req_ready),.req_addr(req_addr),.req_write(req_write),
            .req_wdata(req_wdata),.req_wstrb(req_wstrb),
            .rsp_valid(rsp_valid),.rsp_ready(rsp_ready),.rsp_data(rsp_data),.rsp_error(rsp_error),
            .axi_awaddr(axi_awaddr),.axi_araddr(axi_araddr),.axi_awvalid(axi_awvalid),.axi_arvalid(axi_arvalid),
            .axi_awready(axi_awready),.axi_arready(axi_arready),.axi_wdata(axi_wdata),.axi_wstrb(axi_wstrb),
            .axi_wready(axi_wready),.axi_wusero_last(axi_wusero_last),.axi_wusero_id(axi_wusero_id),
            .axi_rdata(axi_rdata),.axi_rvalid(axi_rvalid),.axi_rlast(axi_rlast),.axi_rid(axi_rid));
    end else begin: queued
        wire [27:0] read_addr;wire read_valid,read_ready;
        wire [255:0] read_data;wire read_response,read_last;wire [3:0] read_id;
        if(MAX_READ_BURST>1)begin: burst
            ag_hmic_read_burst #(.DEPTH(DEPTH),.MAX_BURST(MAX_READ_BURST)) coalesce(
                .clk(ui_clk),.reset_n(reset_n),.enable(init_done),
                .in_araddr(read_addr),.in_arvalid(read_valid),.in_arready(read_ready),
                .out_araddr(axi_araddr),.out_arlen(axi_arlen),.out_arvalid(axi_arvalid),.out_arready(axi_arready),
                .out_rdata(axi_rdata),.out_rvalid(axi_rvalid),.out_rlast(axi_rlast),.out_rid(axi_rid),
                .in_rdata(read_data),.in_rvalid(read_response),.in_rlast(read_last),.in_rid(read_id));
        end else begin: single_read
            assign axi_araddr=read_addr;assign axi_arvalid=read_valid;assign read_ready=axi_arready;assign axi_arlen=0;
            assign read_data=axi_rdata;assign read_response=axi_rvalid;assign read_last=axi_rlast;assign read_id=axi_rid;
        end
        ag_hmic_bridge_queue4 #(.TIMEOUT_BITS(TIMEOUT_BITS),.DEPTH(DEPTH)) impl(
            .cpu_clk(cpu_clk),.ui_clk(ui_clk),.reset_n(reset_n),.init_done(init_done),
            .ready_cpu(ready_cpu),.fault_cpu(fault_cpu),
            .req_valid(req_valid),.req_ready(req_ready),.req_addr(req_addr),.req_write(req_write),
            .req_wdata(req_wdata),.req_wstrb(req_wstrb),
            .rsp_valid(rsp_valid),.rsp_ready(rsp_ready),.rsp_data(rsp_data),.rsp_error(rsp_error),
            .axi_awaddr(axi_awaddr),.axi_araddr(read_addr),.axi_awvalid(axi_awvalid),.axi_arvalid(read_valid),
            .axi_awready(axi_awready),.axi_arready(read_ready),.axi_wdata(axi_wdata),.axi_wstrb(axi_wstrb),
            .axi_wready(axi_wready),.axi_wusero_last(axi_wusero_last),.axi_wusero_id(axi_wusero_id),
            .axi_rdata(read_data),.axi_rvalid(read_response),.axi_rlast(read_last),.axi_rid(read_id));
        initial if(DEPTH<4||DEPTH>32||(DEPTH&(DEPTH-1))!=0)$error("ag_hmic_bridge requires power-of-two DEPTH1 or4..32");
    end endgenerate
endmodule

// Bundled-data mailbox + Logos2 HMIC_S simplified AXI adapter (UG042003).
// Both domains MUST share reset_n; payload registers remain stable throughout
// the toggle handshake. Constrain their CDC flight time, not just the toggles.
// HMIC has no WVALID, B channel or RREADY. Reserve storage before AR; retain
// WDATA/WSTRB before AW and through WREADY. Completion means controller acceptance.
module ag_hmic_bridge_single #(
    parameter integer TIMEOUT_BITS = 20
) (
    input logic cpu_clk, ui_clk, reset_n,
    input logic init_done,
    output logic ready_cpu, fault_cpu,
    input logic req_valid, output logic req_ready,
    input logic [29:0] req_addr, input logic req_write,
    input logic [255:0] req_wdata, input logic [31:0] req_wstrb,
    output logic rsp_valid, input logic rsp_ready,
    output logic [255:0] rsp_data, output logic rsp_error,
    output logic [27:0] axi_awaddr, axi_araddr,
    output logic axi_awvalid, axi_arvalid,
    input logic axi_awready, axi_arready,
    output logic [255:0] axi_wdata, output logic [31:0] axi_wstrb,
    input logic axi_wready, axi_wusero_last, input logic [3:0] axi_wusero_id,
    input logic [255:0] axi_rdata, input logic axi_rvalid, axi_rlast,
    input logic [3:0] axi_rid
);
    (* async_reg = "true" *) logic [2:0] cpu_reset, ui_reset;
    (* async_reg = "true" *) logic [1:0] req_sync, ack_sync, init_sync, fault_sync;
    logic req_toggle, ack_toggle, busy, local_fault, ui_fault, ever_ready;
    logic [TIMEOUT_BITS-1:0] cpu_timer, ui_timer;
    // Source-domain holding registers, sampled after the associated toggle.
    logic [29:0] hold_addr;
    logic hold_write;
    logic [255:0] hold_wdata, return_data;
    logic [31:0] hold_wstrb;
    logic return_error;
    localparam logic [1:0] IDLE=0, AW=1, W=2, AR_R=3;
    logic [1:0] state;
    logic ar_sent;
    always_ff @(posedge cpu_clk or negedge reset_n)
        if (!reset_n) cpu_reset <= 3'b111;
        else cpu_reset <= {cpu_reset[1:0], 1'b0};
    always_ff @(posedge ui_clk or negedge reset_n)
        if (!reset_n) ui_reset <= 3'b111;
        else ui_reset <= {ui_reset[1:0], 1'b0};
    assign ready_cpu = init_sync[1] && !fault_cpu && !cpu_reset[2];
    assign fault_cpu = local_fault || fault_sync[1];
    assign req_ready = !cpu_reset[2] && !busy && !rsp_valid;
    assign axi_awvalid = state == AW && init_done && !ui_fault && !ui_reset[2];
    assign axi_arvalid = state == AR_R && !ar_sent && init_done && !ui_fault && !ui_reset[2];
    assign axi_araddr = axi_awaddr;
    always_ff @(posedge cpu_clk or negedge reset_n) begin
        if (!reset_n) begin
            ack_sync <= 0; init_sync <= 0; fault_sync <= 0;
        end else begin
            ack_sync <= {ack_sync[0], ack_toggle};
            init_sync <= {init_sync[0], init_done};
            fault_sync <= {fault_sync[0], ui_fault};
        end
    end
    always_ff @(posedge cpu_clk or negedge reset_n) begin
        if (!reset_n) begin
            req_toggle <= 0; busy <= 0; local_fault <= 0; cpu_timer <= 0;
            hold_addr <= 0; hold_write <= 0; hold_wdata <= 0; hold_wstrb <= 0;
            rsp_valid <= 0; rsp_data <= 0; rsp_error <= 0;
        end else if (!cpu_reset[2]) begin
            if (rsp_ready) rsp_valid <= 0;
            if (req_valid && req_ready) begin
                cpu_timer <= 0;
                if (!ready_cpu || req_addr[4:0] != 0) begin
                    rsp_valid <= 1; rsp_data <= 0; rsp_error <= 1;
                end else begin
                    hold_addr <= req_addr; hold_write <= req_write;
                    hold_wdata <= req_wdata; hold_wstrb <= req_wstrb;
                    req_toggle <= !req_toggle; busy <= 1;
                end
            end
            if (busy) begin
                cpu_timer <= cpu_timer + 1'b1;
                if (ack_sync[1] == req_toggle) begin
                    busy <= 0; rsp_valid <= 1; rsp_data <= return_data; rsp_error <= return_error;
                end else if (&cpu_timer) begin
                    // Never reuse a timed-out mailbox: a late DDR response may
                    // still arrive. Only a common controller/bridge reset recovers.
                    busy <= 0; local_fault <= 1; rsp_valid <= 1; rsp_error <= 1; rsp_data <= 0;
                end
            end
        end
    end
    always_ff @(posedge ui_clk or negedge reset_n)
        if (!reset_n) req_sync <= 0;
        else req_sync <= {req_sync[0], req_toggle};

    always_ff @(posedge ui_clk or negedge reset_n) begin
        if (!reset_n) begin
            state <= IDLE; ack_toggle <= 0; return_data <= 0; return_error <= 0;
            ui_fault <= 0; ever_ready <= 0; ui_timer <= 0; ar_sent <= 0;
            axi_awaddr <= 0; axi_wdata <= 0; axi_wstrb <= 0;
        end else if (!ui_reset[2]) begin
            if (init_done) ever_ready <= 1;
            if (ever_ready && !init_done) ui_fault <= 1;
            if (state == IDLE) begin
                ui_timer <= 0; ar_sent <= 0;
                if (req_sync[1] != ack_toggle) begin
                    if (!init_done || ui_fault) begin
                        return_error <= 1; return_data <= 0; ack_toggle <= req_sync[1];
                    end else begin
                        axi_awaddr <= hold_addr[29:2];
                        axi_wdata <= hold_wdata; axi_wstrb <= hold_wstrb;
                        state <= hold_write ? AW : AR_R;
                    end
                end
            end else begin
                ui_timer <= ui_timer + 1'b1;
                if (!init_done || ui_fault || &ui_timer) begin
                    ui_fault <= 1; return_error <= 1; return_data <= 0;
                    ack_toggle <= req_sync[1]; state <= IDLE;
                end else if ((state == AW && axi_awready && axi_wready) || (state == W && axi_wready)) begin
                    return_error <= !axi_wusero_last || axi_wusero_id != 0;
                    ui_fault <= !axi_wusero_last || axi_wusero_id != 0;
                    return_data <= 0; ack_toggle <= req_sync[1]; state <= IDLE;
                end else if (state == AW && axi_awready) state <= W;
                else if (state == AR_R) begin
                    if (axi_arready) ar_sent <= 1;
                    if (axi_rvalid && (ar_sent || axi_arready)) begin
                        return_data <= axi_rdata;
                        return_error <= !axi_rlast || axi_rid != 0;
                        ui_fault <= !axi_rlast || axi_rid != 0;
                        ack_toggle <= req_sync[1]; state <= IDLE;
                    end
                end
            end
        end
    end
endmodule

// Four-entry asynchronous FIFO. Payload is stable in source-owned storage
// before its Gray write pointer can traverse two destination synchronizers.
// reset_n MUST reset both clock domains and the attached controller together.
module ag_hmic_async_fifo4 #(parameter integer WIDTH=32,parameter integer DEPTH=4)(
 input logic wclk,rclk,reset_n,wrst,rrst,
 input logic wvalid,output logic wready,input logic [WIDTH-1:0] wdata,
 output logic rvalid,input logic rready,output logic [WIDTH-1:0] rdata
);
 localparam integer PTR=$clog2(DEPTH);
 logic [WIDTH-1:0] payload[0:DEPTH-1] /* synthesis syn_ramstyle = "select_ram" */;
 logic [PTR:0] wbin,wgray,rbin,rgray;
 (* async_reg="true" *) logic [PTR:0] rgray_w1,rgray_w2,wgray_r1,wgray_r2;
 logic read_valid;
 wire [PTR:0] wnext=wbin+1'b1;
 wire [PTR:0] wnext_gray=(wnext>>1)^wnext;
 wire [PTR:0] rnext=rbin+1'b1;
 wire full=wgray=={~rgray_w2[PTR:PTR-1],rgray_w2[PTR-2:0]};
 wire storage_valid=rgray!=wgray_r2;
 wire refill=!rrst&&(!read_valid||rready)&&storage_valid;
 assign wready=!wrst&&!full;
 assign rvalid=!rrst&&read_valid;
 // The asynchronous storage terminates here, before any consumer control.
 always_ff @(posedge rclk)
  if(refill)rdata<=payload[rbin[PTR-1:0]];
 always_ff @(posedge wclk or negedge reset_n)begin
  if(!reset_n)begin wbin<=0;wgray<=0;rgray_w1<=0;rgray_w2<=0;end
  else if(wrst)begin wbin<=0;wgray<=0;rgray_w1<=0;rgray_w2<=0;end
  else begin
   rgray_w1<=rgray;rgray_w2<=rgray_w1;
   if(wvalid&&wready)begin payload[wbin[PTR-1:0]]<=wdata;wbin<=wnext;wgray<=wnext_gray;end
  end
 end
 always_ff @(posedge rclk or negedge reset_n)begin
  if(!reset_n)begin rbin<=0;rgray<=0;wgray_r1<=0;wgray_r2<=0;read_valid<=0;end
  else if(rrst)begin rbin<=0;rgray<=0;wgray_r1<=0;wgray_r2<=0;read_valid<=0;end
  else begin
   wgray_r1<=wgray;wgray_r2<=wgray_r1;
   if(!read_valid||rready)read_valid<=storage_valid;
   // Return storage credit only once the beat is captured in this domain.
   if(refill)begin rbin<=rnext;rgray<=(rnext>>1)^rnext;end
  end
 end
endmodule

module ag_hmic_bridge_queue4 #(parameter integer TIMEOUT_BITS=20,parameter integer DEPTH=4)(
 input logic cpu_clk,ui_clk,reset_n,init_done,
 output logic ready_cpu,fault_cpu,
 input logic req_valid,output logic req_ready,
 input logic [29:0] req_addr,input logic req_write,
 input logic [255:0] req_wdata,input logic [31:0] req_wstrb,
 output logic rsp_valid,input logic rsp_ready,
 output logic [255:0] rsp_data,output logic rsp_error,
 output logic [27:0] axi_awaddr,axi_araddr,
 output logic axi_awvalid,axi_arvalid,input logic axi_awready,axi_arready,
 output logic [255:0] axi_wdata,output logic [31:0] axi_wstrb,
 input logic axi_wready,axi_wusero_last,input logic [3:0] axi_wusero_id,
 input logic [255:0] axi_rdata,input logic axi_rvalid,axi_rlast,input logic [3:0] axi_rid
);
 (* async_reg="true" *) logic [2:0] cpu_reset,ui_reset;
 (* async_reg="true" *) logic [1:0] init_sync,ufault_sync,cfault_sync;
 logic cpu_fault,ui_fault,ever_ready;
 localparam integer PTR=$clog2(DEPTH);
 logic [PTR:0] credits;
 logic [TIMEOUT_BITS-1:0] cpu_timer,ui_timer;
 logic ui_timeout;
 logic q_wready,q_rvalid,q_rready;
 logic [319:0] q_rdata;
 logic ret_wready,ret_wvalid,ret_rvalid,ret_rready;
 logic [256:0] ret_wdata,ret_rdata;
 wire accept=req_valid&&req_ready;
 wire response_ack=rsp_valid&&rsp_ready;
 wire bad_request=!ready_cpu||req_addr[4:0]!=0;
 always_ff @(posedge cpu_clk or negedge reset_n)
  if(!reset_n)cpu_reset<=3'b111;else cpu_reset<={cpu_reset[1:0],1'b0};
 always_ff @(posedge ui_clk or negedge reset_n)
  if(!reset_n)ui_reset<=3'b111;else ui_reset<={ui_reset[1:0],1'b0};
 always_ff @(posedge cpu_clk or negedge reset_n)begin
  if(!reset_n)begin init_sync<=0;ufault_sync<=0;end
  else begin init_sync<={init_sync[0],init_done};ufault_sync<={ufault_sync[0],ui_fault};end
 end
 always_ff @(posedge ui_clk or negedge reset_n)
  if(!reset_n)cfault_sync<=0;else cfault_sync<={cfault_sync[0],cpu_fault};
 assign fault_cpu=cpu_fault||ufault_sync[1];
 assign ready_cpu=!cpu_reset[2]&&init_sync[1]&&!fault_cpu;
 // Every accepted request reserves one return credit through CPU response ACK.
 // Fault never admits a new task; accepted tasks are still drained in order.
 assign req_ready=!cpu_reset[2]&&!fault_cpu&&credits<DEPTH&&q_wready;
 assign rsp_valid=!cpu_reset[2]&&credits!=0&&(cpu_fault||ret_rvalid);
 assign rsp_error=cpu_fault?1'b1:ret_rdata[256];
 assign rsp_data=cpu_fault?256'd0:ret_rdata[255:0];
 assign ret_rready=!cpu_reset[2]&&!cpu_fault&&credits!=0&&rsp_ready;
 ag_hmic_async_fifo4 #(.WIDTH(320),.DEPTH(DEPTH)) requests(
  .wclk(cpu_clk),.rclk(ui_clk),.reset_n(reset_n),.wrst(cpu_reset[2]),.rrst(ui_reset[2]),
  .wvalid(accept),.wready(q_wready),.wdata({bad_request,req_write,req_addr,req_wstrb,req_wdata}),
  .rvalid(q_rvalid),.rready(q_rready),.rdata(q_rdata));
 ag_hmic_async_fifo4 #(.WIDTH(257),.DEPTH(DEPTH)) responses(
  .wclk(ui_clk),.rclk(cpu_clk),.reset_n(reset_n),.wrst(ui_reset[2]),.rrst(cpu_reset[2]),
  .wvalid(ret_wvalid),.wready(ret_wready),.wdata(ret_wdata),
  .rvalid(ret_rvalid),.rready(ret_rready),.rdata(ret_rdata));
 always_ff @(posedge cpu_clk or negedge reset_n)begin
  if(!reset_n)begin credits<=0;cpu_fault<=0;cpu_timer<=0;end
  else if(!cpu_reset[2])begin
   case({accept,response_ack})2'b10:credits<=credits+1'b1;2'b01:credits<=credits-1'b1;default:begin end endcase
   // Backpressure at the consumer is not a DDR timeout. A held return is
   // stable indefinitely; only waiting for an unavailable head is timed.
   if(credits==0||rsp_valid||response_ack)cpu_timer<=0;
   else if(!cpu_fault)begin
    cpu_timer<=cpu_timer+1'b1;
    if(&cpu_timer)cpu_fault<=1;
   end
   // UI errors are already ordered in its return FIFO. Only CPU-side timeout
   // synthesizes all remaining errors and quarantines the entire old FIFO.
  end
 end
 logic [29:0] slot_addr[0:DEPTH-1];
 logic slot_write[0:DEPTH-1],slot_bad[0:DEPTH-1],slot_done[0:DEPTH-1],slot_error[0:DEPTH-1],slot_return_valid[0:DEPTH-1];
 logic [255:0] slot_data[0:DEPTH-1] /* synthesis syn_ramstyle = "select_ram" */;
 logic [255:0] slot_return[0:DEPTH-1] /* synthesis syn_ramstyle = "select_ram" */;
 logic [31:0] slot_strobe[0:DEPTH-1];
 logic [PTR-1:0] allocate,issue,retire,read_head,read_tail;
 logic [PTR-1:0] read_slots[0:DEPTH-1];
 logic [PTR:0] slot_count,unissued,read_count;
 logic write_wait;
 logic ar_valid_q,aw_valid_q;
 logic [27:0] ar_addr_q,aw_addr_q;
 logic [255:0] write_data_q,read_data_q;
 logic [31:0] write_strobe_q;
 logic read_complete,read_bad_q,write_complete,write_bad_q;
 logic [PTR-1:0] read_complete_slot,write_complete_slot;
 wire [PTR-1:0] next_issue=issue+1'b1;
 wire allocate_fire=q_rvalid&&q_rready;
 wire retire_fire=ret_wvalid&&ret_wready;
 wire issue_available=unissued!=0;
 wire issue_bad=issue_available&&slot_bad[issue];
 wire read_fire=axi_arvalid&&axi_arready;
 wire aw_fire=axi_awvalid&&axi_awready;
 wire write_fire=axi_wready&&(write_wait||aw_fire);
 wire read_response=axi_rvalid&&(read_count!=0||read_fire);
 wire [PTR-1:0] response_slot=read_count!=0?read_slots[read_head]:issue;
 wire fault_now=(ever_ready&&!init_done)||cfault_sync[1]||
      (read_complete&&read_bad_q)||
      (write_complete&&write_bad_q)||
      (slot_count!=0&&!retire_fire&&ui_timeout);
 wire issue_finish=!ui_fault&&!fault_now&&(issue_bad||read_fire||write_fire);
 assign q_rready=!ui_reset[2]&&slot_count<DEPTH;
 assign ret_wvalid=!ui_reset[2]&&slot_count!=0&&slot_done[retire];
 assign ret_wdata={slot_error[retire],slot_return_valid[retire]?slot_return[retire]:256'd0};
 // Writes are barriers: all earlier returns have left local slots before AW,
 // and no later command is issued until W has completed. Read runs pipeline.
 wire command_enable=!ui_reset[2]&&!ui_fault&&!cfault_sync[1]&&init_done;
 assign axi_arvalid=command_enable&&ar_valid_q;
 assign axi_awvalid=command_enable&&aw_valid_q;
 assign axi_araddr=ar_addr_q;assign axi_awaddr=aw_addr_q;
 assign axi_wdata=write_data_q;assign axi_wstrb=write_strobe_q;
 wire load_read_command=command_enable&&!fault_now&&!write_wait&&
      ((!ar_valid_q&&!aw_valid_q&&issue_available&&!slot_bad[issue]&&!slot_write[issue])||
       (read_fire&&unissued>1&&!slot_bad[next_issue]&&!slot_write[next_issue]));
 wire [PTR-1:0] read_command_slot=read_fire?next_issue:issue;
 wire load_write_command=command_enable&&!fault_now&&!write_wait&&!ar_valid_q&&!aw_valid_q&&
      issue_available&&!slot_bad[issue]&&slot_write[issue]&&issue==retire&&read_count==0;
 // Capture controller returns before validation/fault fanout. The raw response
 // still consumes its ordered read-slot tag on its arrival edge.
 always_ff @(posedge ui_clk or negedge reset_n)begin
  if(!reset_n)begin
   ar_valid_q<=0;aw_valid_q<=0;read_complete<=0;write_complete<=0;
   read_bad_q<=0;write_bad_q<=0;read_complete_slot<=0;write_complete_slot<=0;
  end else if(ui_reset[2])begin
   ar_valid_q<=0;aw_valid_q<=0;read_complete<=0;write_complete<=0;
  end else begin
   read_complete<=axi_rvalid&&!ui_fault;
   write_complete<=write_fire&&!ui_fault;
   if(axi_rvalid)begin
    read_bad_q<=!read_response||!axi_rlast||axi_rid!=0;
    read_complete_slot<=response_slot;
   end
   if(write_fire)begin
    write_bad_q<=!axi_wusero_last||axi_wusero_id!=0;
    write_complete_slot<=issue;
   end
   if(ui_fault||fault_now)begin ar_valid_q<=0;aw_valid_q<=0;end
   else begin
    if(read_fire)ar_valid_q<=0;
    if(aw_fire)aw_valid_q<=0;
    if(load_read_command)ar_valid_q<=1;
    if(load_write_command)aw_valid_q<=1;
   end
  end
 end
 // Payload words are written before their valid/done bits become visible.
 // Keep these single-write memories out of asynchronous reset/control clear
 // paths so they infer LUTRAM instead of thousands of resettable flops.
 always_ff @(posedge ui_clk)begin
  if(reset_n&&!ui_reset[2])begin
   if(allocate_fire)begin
    slot_addr[allocate]<=q_rdata[317:288];
    slot_strobe[allocate]<=q_rdata[287:256];
    slot_data[allocate]<=q_rdata[255:0];
   end
   if(load_read_command)ar_addr_q<=slot_addr[read_command_slot][29:2];
   if(load_write_command)begin
    aw_addr_q<=slot_addr[issue][29:2];
    write_data_q<=slot_data[issue];write_strobe_q<=slot_strobe[issue];
   end
   if(axi_rvalid&&!ui_fault)read_data_q<=axi_rdata;
   if(!ui_fault&&!fault_now&&read_complete)
    slot_return[read_complete_slot]<=read_data_q;
  end
 end
 integer s;
 always_ff @(posedge ui_clk or negedge reset_n)begin
  if(!reset_n)begin
   ui_fault<=0;ever_ready<=0;ui_timer<=0;ui_timeout<=0;allocate<=0;issue<=0;retire<=0;
   read_head<=0;read_tail<=0;slot_count<=0;unissued<=0;read_count<=0;write_wait<=0;
   for(s=0;s<DEPTH;s=s+1)begin slot_write[s]<=0;slot_bad[s]<=0;slot_done[s]<=0;slot_error[s]<=0;slot_return_valid[s]<=0;read_slots[s]<=0;end
  end else if(!ui_reset[2])begin
   if(init_done)ever_ready<=1;
   if(slot_count==0||retire_fire||read_response||write_fire)begin ui_timer<=0;ui_timeout<=0;end
   else if(!ui_fault)begin
    if(!ui_timeout)ui_timer<=ui_timer+1'b1;
    ui_timeout<=ui_timeout||(&ui_timer);
   end
   if(allocate_fire)begin
    slot_write[allocate]<=q_rdata[318];
    slot_bad[allocate]<=q_rdata[319]||!init_done||ui_fault||fault_now;
    slot_return_valid[allocate]<=0;slot_error[allocate]<=ui_fault||fault_now;
    slot_done[allocate]<=ui_fault||fault_now;allocate<=allocate+1'b1;
   end
   if(retire_fire)begin slot_done[retire]<=0;retire<=retire+1'b1;end
   case({allocate_fire,retire_fire})2'b10:slot_count<=slot_count+1'b1;2'b01:slot_count<=slot_count-1'b1;default:begin end endcase
   if(ui_fault||fault_now)begin
    ui_fault<=1;write_wait<=0;read_count<=0;unissued<=0;issue<=allocate+{{(PTR-1){1'b0}},allocate_fire};
    // Slots are retired only in allocation order. Physical late HMIC beats
    // are ignored forever after fault; no new task can inherit their data.
    for(s=0;s<DEPTH;s=s+1)begin slot_done[s]<=1;slot_error[s]<=1;slot_return_valid[s]<=0;end
   end else begin
    case({allocate_fire,issue_finish})2'b10:unissued<=unissued+1'b1;2'b01:unissued<=unissued-1'b1;default:begin end endcase
    if(issue_bad)begin slot_done[issue]<=1;slot_error[issue]<=1;slot_return_valid[issue]<=0;end
    if(aw_fire&&!write_fire)write_wait<=1;
    if(write_fire)write_wait<=0;
   if(write_complete)begin slot_done[write_complete_slot]<=1;slot_error[write_complete_slot]<=0;slot_return_valid[write_complete_slot]<=0;end
    if(read_fire)begin read_slots[read_tail]<=issue;read_tail<=read_tail+1'b1;end
    if(read_response)read_head<=read_head+1'b1;
   if(read_complete)begin slot_done[read_complete_slot]<=1;slot_error[read_complete_slot]<=0;slot_return_valid[read_complete_slot]<=1;end
    case({read_fire,read_response})2'b10:read_count<=read_count+1'b1;2'b01:read_count<=read_count-1'b1;default:begin end endcase
    if(issue_finish)issue<=issue+1'b1;
   end
  end
 end
endmodule
