// Production integration shell: current AG SoC, legacy BRAM diagnostics,
// streaming DDR NPU and physical SD SPI pins. DDR PHY remains the board bridge.
module ag_npu_system #(
    parameter INIT_FILE = "firmware.hex",
    parameter integer CLOCK_HZ = 102000000,
    parameter integer TCM_WORDS = 16384,
    parameter bit SIM_FAST_UART = 0,
    parameter bit DUAL_ISSUE = 1,
    parameter bit DDR_ENABLE = 1,
    parameter bit CACHE_ENABLE = DDR_ENABLE,
    parameter integer CACHE_SET_BITS = 8,
    parameter bit MULH_PIPELINE = 1,
    parameter bit LEGACY_ENABLE = 0,
    parameter bit NPU_REUSE_ACT_LINE = 0,
    parameter bit TENSOR_ENABLE = 0,
    parameter integer NPU_LANES = 16,
    parameter integer DDR_OUTSTANDING = 1,
    parameter bit PARTIAL_SRAM_ENABLE = 0,
    parameter bit TENSOR_PARALLEL_RAW = 1,
    parameter bit TENSOR_PARALLEL_DELTA = 1,
    parameter bit TENSOR_SHARED_RAW_ACCUM = 0,
    parameter bit LOCAL_CPU_RESET = 0,
    parameter bit BUTTON_ENABLE = 0,
    parameter bit TEXT_VIDEO_ENABLE = 0,
    parameter bit UART_RX_ENABLE = 0
) (
    input wire clk, rst, irq_external,
    input wire [4:0] buttons_n,
    input wire pixel_clk,pixel_rst,hd_scl_i,hd_sda_i,
    output wire [23:0] pixel_rgb,output wire pixel_hs,pixel_vs,pixel_de,
    output wire hd_scl_low,hd_sda_low,hd_reset_n,
    input wire uart_rx_i,output wire uart_tx_o,
    output wire [31:0] gpio,
    output wire console_valid,
    output wire [7:0] console_data,
    output wire done,
    output wire [31:0] exit_code,
    output wire trap_valid,
    output wire [31:0] trap_pc, trap_cause, trap_value,
    output wire retire_valid, retire_second_valid,
    output wire [31:0] retire_pc, retire_second_pc,
    output wire [63:0] cycle_count, instret_count,
    output wire [31:0] dual_retire_count,
    input wire ddr_ready, ddr_fault,
    output wire mem_req_valid, input wire mem_req_ready,
    output wire [29:0] mem_req_addr, output wire mem_req_write,
    output wire [255:0] mem_req_wdata, output wire [31:0] mem_req_wstrb,
    input wire mem_rsp_valid, output wire mem_rsp_ready,
    input wire [255:0] mem_rsp_data, input wire mem_rsp_error,
    output wire sd_clk, sd_mosi, sd_cs_n,
    input wire sd_miso, sd_card_present,
    output wire npu_busy, npu_done, npu_error,
    output wire [31:0] npu_error_code, npu_cycles, npu_results,
    output wire ddr_transaction_active, ddr_transaction_owner,
    output reg [31:0] peripheral_requests, legacy_completions,
    output reg [31:0] dma_completions
);
    wire qv, qr, qw, rv, rr, re;
    wire [31:0] qa, qd, rd;
    wire [3:0] qs;
    wire cpu_qv, cpu_qr, cpu_qw, cpu_rv, cpu_rr, cpu_re;
    wire [29:0] cpu_qa;
    wire [255:0] cpu_qd, cpu_rd;
    wire [31:0] cpu_qs;
    wire dma_qv, dma_qr, dma_qw, dma_rv, dma_rr, dma_re;
    wire [29:0] dma_qa;
    wire [255:0] dma_qd, dma_rd;
    wire [31:0] dma_qs;
    wire tensor_qv,tensor_qr,tensor_qw,tensor_rv,tensor_rr,tensor_re;
    wire [29:0] tensor_qa;
    wire [255:0] tensor_qd,tensor_rd;
    wire [31:0] tensor_qs;
    wire accel_qv,accel_qr,accel_qw,accel_rv,accel_rr,accel_re;
    wire [29:0] accel_qa;
    wire [255:0] accel_qd,accel_rd;
    wire [31:0] accel_qs;
    wire shared_qv,shared_qr,shared_qw,shared_rv,shared_rr,shared_re;
    wire [29:0] shared_qa;wire [255:0] shared_qd,shared_rd;wire [31:0] shared_qs;
    wire pqr,prv,pre;wire [31:0] prd;

    // Optional local reset: asynchronous assertion, synchronous release. CPU
    // traffic starts one edge after the peripheral fabric leaves reset.
    wire cpu_reset;
    generate if(LOCAL_CPU_RESET)begin: cpu_reset_distribution
        reg local_reset=1'b1 /* synthesis syn_preserve = 1 */;
        always @(posedge clk or posedge rst)
            if(rst)local_reset<=1'b1;else local_reset<=1'b0;
        assign cpu_reset=local_reset;
    end else begin: direct_cpu_reset
        assign cpu_reset=rst;
    end endgenerate
    ag_soc #(.RAM_WORDS(TCM_WORDS), .INIT_FILE(INIT_FILE), .CLOCK_HZ(CLOCK_HZ),
        .SIM_FAST_UART(SIM_FAST_UART), .DUAL_ISSUE(DUAL_ISSUE),
        .DDR_ENABLE(DDR_ENABLE), .CACHE_ENABLE(CACHE_ENABLE),
        .CACHE_SET_BITS(CACHE_SET_BITS), .MULH_PIPELINE(MULH_PIPELINE),
        .EXT_MMIO_ENABLE(1)) cpu_soc (
        .clk(clk), .rst(cpu_reset), .irq_external(irq_external),
        .uart_tx_o(uart_tx_o), .gpio(gpio),
        .console_valid(console_valid), .console_data(console_data),
        .done(done), .exit_code(exit_code),
        .retire_valid(retire_valid), .retire_pc(retire_pc),
        .retire_insn(), .retire_data(), .retire_rd(),
        .retire_second_valid(retire_second_valid), .retire_second_pc(retire_second_pc),
        .retire_second_insn(), .retire_second_data(), .retire_second_rd(),
        .dual_retire_count(dual_retire_count), .trap_valid(trap_valid),
        .trap_pc(trap_pc), .trap_cause(trap_cause), .trap_value(trap_value),
        .cycle_count(cycle_count), .instret_count(instret_count),
        .branch_count(), .mispredict_count(), .load_stall_count(),
        .ddr_ready(ddr_ready), .ddr_fault(ddr_fault),
        .mem_req_valid(cpu_qv), .mem_req_ready(cpu_qr), .mem_req_addr(cpu_qa),
        .mem_req_write(cpu_qw), .mem_req_wdata(cpu_qd), .mem_req_wstrb(cpu_qs),
        .mem_rsp_valid(cpu_rv), .mem_rsp_ready(cpu_rr),
        .mem_rsp_data(cpu_rd), .mem_rsp_error(cpu_re),
        .ext_req_valid(qv), .ext_req_ready(qr), .ext_req_write(qw),
        .ext_req_addr(qa), .ext_req_wdata(qd), .ext_req_wstrb(qs),
        .ext_rsp_valid(rv), .ext_rsp_ready(rr), .ext_rsp_data(rd), .ext_rsp_error(re)
    );

    wire legacy_select = LEGACY_ENABLE &&
                        ((qa >= 32'h10001000 && qa < 32'h10001100) ||
                         (qa >= 32'h10010000 && qa < 32'h10018000));
    wire dma_select = qa >= 32'h10004000 && qa < 32'h10005000;
    wire sd_select = qa >= 32'h10005000 && qa < 32'h10006000;
    wire tensor_select = TENSOR_ENABLE && qa >= 32'h10006000 && qa < 32'h10007000;
    wire partial_select=PARTIAL_SRAM_ENABLE&&qa>=32'h10007000&&qa<32'h10008000;
    wire lqr, lrv, lre, dqr, drv, dre, sqr, srv, sre,tqr,trv,tre;
    wire bqr,brv,bre,vqr,vrv,vre,uqr,urv,ure;wire [31:0] brd,vrd,urd;
    wire uart_rx_select=UART_RX_ENABLE&&qa>=32'h10001240&&qa<32'h10001250;
    wire video_select=TEXT_VIDEO_ENABLE&&qa>=32'h10001100&&qa<32'h10001140;
    wire button_select=BUTTON_ENABLE&&qa>=32'h10001210&&qa<32'h10001220;
    wire [31:0] lrd, drd, srd,trd;
    reg pending;
    reg [3:0] target;
    wire [3:0] selected = legacy_select ? 4'd0 : dma_select ? 4'd1 :
                         sd_select ? 4'd2 : tensor_select ? 4'd3 : partial_select ? 4'd4 : button_select ? 4'd5 : video_select ? 4'd6 : uart_rx_select ? 4'd7 : 4'd8;
    assign qr = !rst && !pending && (selected==0 ? lqr : selected==1 ? dqr : selected==2 ? sqr : selected==3 ? tqr : selected==4 ? pqr : selected==5 ? bqr : selected==6 ? vqr : selected==7 ? uqr : 1'b1);
    assign rv = !rst && pending && (target==0 ? lrv : target==1 ? drv : target==2 ? srv : target==3 ? trv : target==4 ? prv : target==5 ? brv : target==6 ? vrv : target==7 ? urv : 1'b1);
    assign rd = target==0 ? lrd : target==1 ? drd : target==2 ? srd : target==3 ? trd : target==4 ? prd : target==5 ? brd : target==6 ? vrd : target==7 ? urd : 32'd0;
    assign re = target==0 ? lre : target==1 ? dre : target==2 ? sre : target==3 ? tre : target==4 ? pre : target==5 ? bre : target==6 ? vre : target==7 ? ure : 1'b1;
    wire legacy_busy, legacy_done, legacy_error;
    wire [31:0] legacy_code, legacy_results, legacy_cycles;
    generate if (LEGACY_ENABLE) begin : legacy_diagnostics
    npu_bram_top legacy (
        .clk(clk), .rst_n(!rst),
        .req_valid(qv && !pending && selected==0), .req_ready(lqr),
        .req_write(qw), .req_addr(qa), .req_wdata(qd), .req_wstrb(qs),
        .rsp_valid(lrv), .rsp_ready(rr && pending && target==0),
        .rsp_rdata(lrd), .rsp_error(lre), .busy(legacy_busy),
        .done(legacy_done), .error(legacy_error), .error_code(legacy_code),
        .result_count(legacy_results), .cycle_count(legacy_cycles)
    );
    end else begin : no_legacy
        assign lqr=1'b1; assign lrv=1'b0; assign lrd=32'd0; assign lre=1'b1;
        assign legacy_busy=1'b0; assign legacy_done=1'b0; assign legacy_error=1'b0;
        assign legacy_code=32'd0; assign legacy_results=32'd0; assign legacy_cycles=32'd0;
    end endgenerate
    npu_dma_top #(.REUSE_ACT_LINE(NPU_REUSE_ACT_LINE),.LANES(NPU_LANES),
        .WEIGHT_DEPTH(DDR_OUTSTANDING>=4?DDR_OUTSTANDING:4)) dma (
        .clk(clk), .rst(rst),
        .req_valid(qv && !pending && selected==1), .req_ready(dqr),
        .req_write(qw), .req_addr(qa), .req_wdata(qd), .req_wstrb(qs),
        .rsp_valid(drv), .rsp_ready(rr && pending && target==1),
        .rsp_rdata(drd), .rsp_error(dre),
        .mem_req_valid(dma_qv), .mem_req_ready(dma_qr), .mem_req_addr(dma_qa),
        .mem_req_write(dma_qw), .mem_req_wdata(dma_qd), .mem_req_wstrb(dma_qs),
        .mem_rsp_valid(dma_rv), .mem_rsp_ready(dma_rr),
        .mem_rsp_data(dma_rd), .mem_rsp_error(dma_re),
        .busy(npu_busy), .done(npu_done), .error(npu_error),
        .error_code(npu_error_code), .cycle_count(npu_cycles), .result_count(npu_results)
    );
    reg [1:0] sd_miso_sync, sd_present_sync;
    always @(posedge clk) begin
        if (rst) begin sd_miso_sync<=2'b11; sd_present_sync<=2'b00; end
        else begin
            sd_miso_sync<={sd_miso_sync[0],sd_miso};
            sd_present_sync<={sd_present_sync[0],sd_card_present};
        end
    end
    sd_spi_top sd (
        .clk(clk), .rst(rst),
        .req_valid(qv && !pending && selected==2), .req_ready(sqr),
        .req_write(qw), .req_addr(qa), .req_wdata(qd), .req_wstrb(qs),
        .rsp_valid(srv), .rsp_ready(rr && pending && target==2),
        .rsp_rdata(srd), .rsp_error(sre),
        .spi_clk(sd_clk), .spi_mosi(sd_mosi), .spi_cs_n(sd_cs_n),
        .spi_miso(sd_miso_sync[1]), .card_present(sd_present_sync[1])
    );
    generate if(TENSOR_ENABLE) begin : tensor_v2_enabled
        wire tensor_busy,tensor_done,tensor_error;
        wire [31:0] tensor_error_code,tensor_cycles,tensor_results;
        wire accelerator_active,accelerator_owner;
        npu_tensor_v2 #(.PARALLEL_RAW(TENSOR_PARALLEL_RAW),
            .PARALLEL_DELTA(TENSOR_PARALLEL_DELTA),
            .SHARED_RAW_ACCUM(TENSOR_SHARED_RAW_ACCUM),
            .DELTA_PREFETCH(DDR_OUTSTANDING>=4?DDR_OUTSTANDING:4)) tensor (
            .clk(clk),.rst(rst),
            .req_valid(qv && !pending && selected==3),.req_ready(tqr),
            .req_write(qw),.req_addr(qa),.req_wdata(qd),.req_wstrb(qs),
            .rsp_valid(trv),.rsp_ready(rr && pending && target==3),.rsp_rdata(trd),.rsp_error(tre),
            .mem_req_valid(tensor_qv),.mem_req_ready(tensor_qr),.mem_req_addr(tensor_qa),
            .mem_req_write(tensor_qw),.mem_req_wdata(tensor_qd),.mem_req_wstrb(tensor_qs),
            .mem_rsp_valid(tensor_rv),.mem_rsp_ready(tensor_rr),.mem_rsp_data(tensor_rd),.mem_rsp_error(tensor_re),
            .busy(tensor_busy),.done(tensor_done),.error(tensor_error),
            .error_code(tensor_error_code),.cycle_count(tensor_cycles),.result_count(tensor_results)
        );
        // Fair between complete matrix/tensor transfers. The outer arbiter
        // remains fair to CPU/cache traffic and holds ownership until ACK.
        ddr_arbiter2 #(.DEPTH(DDR_OUTSTANDING)) accelerator_arbiter (
            .clk(clk),.rst(rst),
            .a_req_valid(dma_qv),.a_req_ready(dma_qr),.a_req_addr(dma_qa),
            .a_req_write(dma_qw),.a_req_wdata(dma_qd),.a_req_wstrb(dma_qs),
            .a_rsp_valid(dma_rv),.a_rsp_ready(dma_rr),.a_rsp_data(dma_rd),.a_rsp_error(dma_re),
            .b_req_valid(tensor_qv),.b_req_ready(tensor_qr),.b_req_addr(tensor_qa),
            .b_req_write(tensor_qw),.b_req_wdata(tensor_qd),.b_req_wstrb(tensor_qs),
            .b_rsp_valid(tensor_rv),.b_rsp_ready(tensor_rr),.b_rsp_data(tensor_rd),.b_rsp_error(tensor_re),
            .mem_req_valid(accel_qv),.mem_req_ready(accel_qr),.mem_req_addr(accel_qa),
            .mem_req_write(accel_qw),.mem_req_wdata(accel_qd),.mem_req_wstrb(accel_qs),
            .mem_rsp_valid(accel_rv),.mem_rsp_ready(accel_rr),.mem_rsp_data(accel_rd),.mem_rsp_error(accel_re),
            .active(accelerator_active),.owner(accelerator_owner)
        );
    end else begin : no_tensor_v2
        assign tqr=1'b1;assign trv=1'b0;assign trd=32'd0;assign tre=1'b1;
        assign accel_qv=dma_qv;assign dma_qr=accel_qr;assign accel_qa=dma_qa;
        assign accel_qw=dma_qw;assign accel_qd=dma_qd;assign accel_qs=dma_qs;
        assign dma_rv=accel_rv;assign accel_rr=dma_rr;assign dma_rd=accel_rd;assign dma_re=accel_re;
        assign tensor_qv=1'b0;assign tensor_qa=30'd0;assign tensor_qw=1'b0;
        assign tensor_qd=256'd0;assign tensor_qs=32'd0;assign tensor_rr=1'b0;
        assign tensor_qr=1'b0;assign tensor_rv=1'b0;assign tensor_rd=256'd0;assign tensor_re=1'b0;
    end endgenerate
    ddr_arbiter2 #(.DEPTH(DDR_OUTSTANDING)) arbiter (
        .clk(clk), .rst(rst),
        .a_req_valid(cpu_qv), .a_req_ready(cpu_qr), .a_req_addr(cpu_qa),
        .a_req_write(cpu_qw), .a_req_wdata(cpu_qd), .a_req_wstrb(cpu_qs),
        .a_rsp_valid(cpu_rv), .a_rsp_ready(cpu_rr), .a_rsp_data(cpu_rd), .a_rsp_error(cpu_re),
        .b_req_valid(accel_qv), .b_req_ready(accel_qr), .b_req_addr(accel_qa),
        .b_req_write(accel_qw), .b_req_wdata(accel_qd), .b_req_wstrb(accel_qs),
        .b_rsp_valid(accel_rv), .b_rsp_ready(accel_rr), .b_rsp_data(accel_rd), .b_rsp_error(accel_re),
        .mem_req_valid(shared_qv), .mem_req_ready(shared_qr), .mem_req_addr(shared_qa),
        .mem_req_write(shared_qw), .mem_req_wdata(shared_qd), .mem_req_wstrb(shared_qs),
        .mem_rsp_valid(shared_rv), .mem_rsp_ready(shared_rr),
        .mem_rsp_data(shared_rd), .mem_rsp_error(shared_re),
        .active(ddr_transaction_active), .owner(ddr_transaction_owner)
    );
    generate if(PARTIAL_SRAM_ENABLE)begin: partial_sram_enabled
      npu_partial_sram #(.DDR_DEPTH(DDR_OUTSTANDING)) partial_store(
       .clk(clk),.rst(rst),.req_valid(qv&&!pending&&selected==4),.req_ready(pqr),.req_write(qw),
       .req_addr(qa),.req_wdata(qd),.req_wstrb(qs),.rsp_valid(prv),.rsp_ready(rr&&pending&&target==4),.rsp_rdata(prd),.rsp_error(pre),
       .bus_req_valid(shared_qv),.bus_req_ready(shared_qr),.bus_req_addr(shared_qa),.bus_req_write(shared_qw),.bus_req_wdata(shared_qd),.bus_req_wstrb(shared_qs),
       .bus_rsp_valid(shared_rv),.bus_rsp_ready(shared_rr),.bus_rsp_data(shared_rd),.bus_rsp_error(shared_re),
       .mem_req_valid(mem_req_valid),.mem_req_ready(mem_req_ready),.mem_req_addr(mem_req_addr),.mem_req_write(mem_req_write),.mem_req_wdata(mem_req_wdata),.mem_req_wstrb(mem_req_wstrb),
       .mem_rsp_valid(mem_rsp_valid),.mem_rsp_ready(mem_rsp_ready),.mem_rsp_data(mem_rsp_data),.mem_rsp_error(mem_rsp_error));
    end else begin: no_partial_sram
      assign mem_req_valid=shared_qv;assign shared_qr=mem_req_ready;assign mem_req_addr=shared_qa;assign mem_req_write=shared_qw;
      assign mem_req_wdata=shared_qd;assign mem_req_wstrb=shared_qs;assign shared_rv=mem_rsp_valid;assign mem_rsp_ready=shared_rr;
      assign shared_rd=mem_rsp_data;assign shared_re=mem_rsp_error;
      assign pqr=1'b1;assign prv=1'b0;assign prd=32'd0;assign pre=1'b1;
    end endgenerate
    generate if(UART_RX_ENABLE)begin: board_uart_rx
      ag_uart_rx_top #(.CLOCK_HZ(CLOCK_HZ)) receiver(
       .clk(clk),.rst(rst),.rx_i(uart_rx_i),
       .req_valid(qv&&!pending&&selected==7),.req_ready(uqr),.req_write(qw),
       .req_addr(qa),.req_wdata(qd),.req_wstrb(qs),.rsp_valid(urv),.rsp_ready(rr&&pending&&target==7),.rsp_rdata(urd),.rsp_error(ure));
    end else begin: no_board_uart_rx
      assign uqr=1;assign urv=0;assign urd=0;assign ure=1;
    end endgenerate
    generate if(TEXT_VIDEO_ENABLE)begin: board_text_video
      ag_text_video #(.WAIT_CYCLES(CLOCK_HZ)) display(
       .clk(clk),.rst(rst),.pixel_clk(pixel_clk),.pixel_rst(pixel_rst),
       .req_valid(qv&&!pending&&selected==6),.req_ready(vqr),.req_write(qw),
       .req_addr(qa),.req_wdata(qd),.req_wstrb(qs),.rsp_valid(vrv),.rsp_ready(rr&&pending&&target==6),.rsp_rdata(vrd),.rsp_error(vre),
       .pixel_rgb(pixel_rgb),.pixel_hs(pixel_hs),.pixel_vs(pixel_vs),.pixel_de(pixel_de),
       .hd_scl_i(hd_scl_i),.hd_sda_i(hd_sda_i),.hd_scl_low(hd_scl_low),.hd_sda_low(hd_sda_low),.hd_reset_n(hd_reset_n));
    end else begin: no_board_text_video
      assign vqr=1;assign vrv=0;assign vrd=0;assign vre=1;
      assign pixel_rgb=0;assign pixel_hs=1;assign pixel_vs=1;assign pixel_de=0;
      assign hd_scl_low=0;assign hd_sda_low=0;assign hd_reset_n=0;
    end endgenerate
    generate if(BUTTON_ENABLE)begin: board_buttons
      ag_button_top #(.CLOCK_HZ(CLOCK_HZ)) keys(
       .clk(clk),.rst(rst),.buttons_n(buttons_n),
       .req_valid(qv&&!pending&&selected==5),.req_ready(bqr),.req_write(qw),
       .req_addr(qa),.req_wdata(qd),.req_wstrb(qs),
       .rsp_valid(brv),.rsp_ready(rr&&pending&&target==5),.rsp_rdata(brd),.rsp_error(bre));
    end else begin: no_board_buttons
      assign bqr=1;assign brv=0;assign brd=0;assign bre=1;
    end endgenerate
    reg last_legacy_done, last_dma_done;
    always @(posedge clk) begin
        if (rst) begin
            pending <= 0; target <= 0;
            peripheral_requests <= 0; legacy_completions <= 0; dma_completions <= 0;
            last_legacy_done <= 0; last_dma_done <= 0;
        end else begin
            last_legacy_done <= legacy_done; last_dma_done <= npu_done;
            if (legacy_done && !last_legacy_done) legacy_completions <= legacy_completions+1;
            if (npu_done && !last_dma_done) dma_completions <= dma_completions+1;
            if (rv && rr) pending <= 0;
            if (qv && qr) begin
                pending <= 1; target <= selected;
                peripheral_requests <= peripheral_requests+1;
            end
        end
    end
endmodule
