// MES2L676-100HP: CPU 102 MHz, DDR3 1000 MT/s, HMIC UI 125 MHz.
// This is a separate integration profile; the TCM CoreMark build is unchanged.
module qinglong_ddr_board_top #(
    parameter integer CLOCK_MHZ = 102,
    parameter bit BOARD_MULH_PIPELINE = 0,
    parameter bit BOARD_CACHE_ENABLE = 1,
    parameter bit BOARD_LOAD_WORD_ONLY = 0,
    parameter integer BOARD_CACHE_SET_BITS = 8
) (
    input wire clk_27m, reset_n, ref_clk_p, ref_clk_n,
    output wire uart_txd, output wire [7:0] led,
    output wire mem_cs_n, mem_rst_n, mem_ck, mem_ck_n, mem_cke,
    output wire mem_ras_n, mem_cas_n, mem_we_n, mem_odt,
    output wire [14:0] mem_a, output wire [2:0] mem_ba,
    inout wire [3:0] mem_dqs, mem_dqs_n,
    inout wire [31:0] mem_dq, output wire [3:0] mem_dm
);
    wire cpu_clk, cpu_locked, ui_clk, ref_clk, init_done;
    wire common_reset_n = reset_n && cpu_locked;
    logic [2:0] reset_sync = 3'b111;
    wire ddr_ready, ddr_fault, done;
    wire [31:0] gpio, exit_code;
    wire req_valid, req_ready, req_write, rsp_valid, rsp_ready, rsp_error;
    wire [29:0] req_addr;
    wire [255:0] req_wdata, rsp_data;
    wire [31:0] req_wstrb;
    wire [27:0] axi_awaddr, axi_araddr;
    wire axi_awvalid, axi_awready, axi_arvalid, axi_arready;
    wire [255:0] axi_wdata, axi_rdata;
    wire [31:0] axi_wstrb;
    wire axi_wready, axi_wusero_last, axi_rvalid, axi_rlast;
    wire [3:0] axi_wusero_id, axi_rid;
    qinglong_board_clock #(.CLOCK_MHZ(CLOCK_MHZ)) clocking_unit (
        .clk_27m(clk_27m), .cpu_clk(cpu_clk), .locked(cpu_locked));
    GTP_INBUFDS refclk_inbuf (.I(ref_clk_p), .IB(ref_clk_n), .O(ref_clk));
    always_ff @(posedge cpu_clk or negedge common_reset_n)
        if (!common_reset_n) reset_sync <= 3'b111;
        else reset_sync <= {reset_sync[1:0], 1'b0};
    assign led = {ddr_fault, done && exit_code != 0, 3'b0, ddr_ready, done && exit_code == 0, gpio[0]};
    qinglong_soc #(.CLOCK_HZ(CLOCK_MHZ*1000000), .INIT_FILE("firmware.hex"), .DDR_ENABLE(1),
                  .MULH_PIPELINE(BOARD_MULH_PIPELINE), .CACHE_ENABLE(BOARD_CACHE_ENABLE),
                  .CACHE_SET_BITS(BOARD_CACHE_SET_BITS), .LOAD_WORD_ONLY(BOARD_LOAD_WORD_ONLY)) soc (
        .clk(cpu_clk), .rst(reset_sync[2]), .irq_external(1'b0),
        .uart_tx_o(uart_txd), .gpio(gpio), .done(done), .exit_code(exit_code),
        .ddr_ready(ddr_ready), .ddr_fault(ddr_fault),
        .mem_req_valid(req_valid), .mem_req_ready(req_ready), .mem_req_addr(req_addr),
        .mem_req_write(req_write), .mem_req_wdata(req_wdata), .mem_req_wstrb(req_wstrb),
        .mem_rsp_valid(rsp_valid), .mem_rsp_ready(rsp_ready), .mem_rsp_data(rsp_data), .mem_rsp_error(rsp_error)
    );
    qinglong_hmic_bridge bridge (
        .cpu_clk(cpu_clk), .ui_clk(ui_clk), .reset_n(common_reset_n), .init_done(init_done),
        .ready_cpu(ddr_ready), .fault_cpu(ddr_fault),
        .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr), .req_write(req_write),
        .req_wdata(req_wdata), .req_wstrb(req_wstrb),
        .rsp_valid(rsp_valid), .rsp_ready(rsp_ready), .rsp_data(rsp_data), .rsp_error(rsp_error),
        .axi_awaddr(axi_awaddr), .axi_awvalid(axi_awvalid), .axi_awready(axi_awready),
        .axi_araddr(axi_araddr), .axi_arvalid(axi_arvalid), .axi_arready(axi_arready),
        .axi_wdata(axi_wdata), .axi_wstrb(axi_wstrb), .axi_wready(axi_wready),
        .axi_wusero_last(axi_wusero_last), .axi_wusero_id(axi_wusero_id),
        .axi_rdata(axi_rdata), .axi_rvalid(axi_rvalid), .axi_rlast(axi_rlast), .axi_rid(axi_rid)
    );
    // Preserve vendor instance name so the generated PHY constraints apply.
    qinglong_ddr3 I_ips_ddr_top (
        .ref_clk(ref_clk), .resetn(common_reset_n), .core_clk(ui_clk), .ddr_init_done(init_done),
        .axi_awaddr(axi_awaddr), .axi_awuser_ap(1'b0), .axi_awuser_id(4'b0), .axi_awlen(4'b0),
        .axi_awready(axi_awready), .axi_awvalid(axi_awvalid), .axi_wdata(axi_wdata),
        .axi_wstrb(axi_wstrb), .axi_wready(axi_wready),
        .axi_wusero_id(axi_wusero_id), .axi_wusero_last(axi_wusero_last),
        .axi_araddr(axi_araddr), .axi_aruser_ap(1'b0), .axi_aruser_id(4'b0), .axi_arlen(4'b0),
        .axi_arready(axi_arready), .axi_arvalid(axi_arvalid), .axi_rdata(axi_rdata),
        .axi_rid(axi_rid), .axi_rlast(axi_rlast), .axi_rvalid(axi_rvalid),
        .apb_clk(1'b0), .apb_rst_n(1'b0), .apb_sel(1'b0), .apb_enable(1'b0),
        .apb_addr(8'b0), .apb_write(1'b0), .apb_wdata(16'b0),
        .mem_cs_n(mem_cs_n), .mem_rst_n(mem_rst_n), .mem_ck(mem_ck), .mem_ck_n(mem_ck_n),
        .mem_cke(mem_cke), .mem_ras_n(mem_ras_n), .mem_cas_n(mem_cas_n), .mem_we_n(mem_we_n),
        .mem_odt(mem_odt), .mem_a(mem_a), .mem_ba(mem_ba), .mem_dqs(mem_dqs),
        .mem_dqs_n(mem_dqs_n), .mem_dq(mem_dq), .mem_dm(mem_dm),
        .dbg_gate_start(1'b0), .dbg_cpd_start(1'b0), .dbg_ddrphy_rst_n(1'b1), .dbg_gpll_scan_rst(1'b0),
        .samp_position_dyn_adj(1'b0), .init_samp_position_even(32'b0), .init_samp_position_odd(32'b0),
        .wrcal_position_dyn_adj(1'b0), .init_wrcal_position(32'b0), .force_read_clk_ctrl(1'b0),
        .init_slip_step(16'b0), .init_read_clk_ctrl(12'b0), .debug_cpd_offset_adj(1'b0),
        .debug_cpd_offset_dir(1'b0), .debug_cpd_offset(10'b0), .ck_dly_en(1'b1), .init_ck_dly_step(8'b0)
    );
endmodule
