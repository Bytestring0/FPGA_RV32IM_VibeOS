// Bundled-data mailbox + Logos2 HMIC_S simplified AXI adapter (UG042003).
// Both domains MUST share reset_n; payload registers remain stable throughout
// the toggle handshake. Constrain their CDC flight time, not just the toggles.
// HMIC has no WVALID, B channel or RREADY. Reserve storage before AR; retain
// WDATA/WSTRB before AW and through WREADY. Completion means controller acceptance.
module ag_hmic_bridge #(
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
