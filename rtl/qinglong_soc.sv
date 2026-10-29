// Dual-port TCM + optional uncached DDR window. Default remains TCM-only.
module qinglong_soc #(
    parameter integer RAM_WORDS = 16384,
    parameter INIT_FILE = "build/firmware.hex",
    parameter integer CLOCK_HZ = 50000000,
    parameter bit SIM_FAST_UART = 0,
    parameter bit PREDICT_ENABLE = 1,
    parameter integer BTB_BITS = 8,
    parameter bit DUAL_ISSUE = 1,
    parameter bit LOAD_BYPASS = 1,
    parameter bit LOAD_WORD_ONLY = 0,
    parameter bit EARLY_LSU = 1,
    parameter bit DDR_ENABLE = 0,
    parameter bit CACHE_ENABLE = DDR_ENABLE,
    parameter integer CACHE_SET_BITS = 7,
    parameter bit PIPELINED_MMIO = DDR_ENABLE,
    parameter bit MULH_PIPELINE = 0
) (
    input logic clk, rst,
    input logic irq_external,
    output logic uart_tx_o,
    output logic [31:0] gpio,
    output logic console_valid,
    output logic [7:0] console_data,
    output logic done,
    output logic [31:0] exit_code,
    output logic retire_valid,
    output logic [31:0] retire_pc, retire_insn, retire_data,
    output logic [4:0] retire_rd,
    output logic retire_second_valid,
    output logic [31:0] retire_second_pc, retire_second_insn, retire_second_data,
    output logic [4:0] retire_second_rd,
    output logic [31:0] dual_retire_count,
    output logic trap_valid,
    output logic [31:0] trap_pc, trap_cause, trap_value,
    output logic [63:0] cycle_count, instret_count,
    output logic [31:0] branch_count, mispredict_count, load_stall_count,
    input logic ddr_ready, ddr_fault,
    output logic mem_req_valid, input logic mem_req_ready,
    output logic [29:0] mem_req_addr, output logic mem_req_write,
    output logic [255:0] mem_req_wdata, output logic [31:0] mem_req_wstrb,
    input logic mem_rsp_valid, output logic mem_rsp_ready,
    input logic [255:0] mem_rsp_data, input logic mem_rsp_error
);
    localparam integer AW = $clog2(RAM_WORDS);
    logic [31:0] ram [0:RAM_WORDS-1];
    logic iv, ir, irv, irr;
    logic [1:0] ire;
    logic [31:0] ia;
    logic [63:0] idata;
    logic dv, dr, dw, drv, drr, dre;
    logic [31:0] da, dd, drdata;
    logic [3:0] ds;
    logic [31:0] ram_i_data, ram_i_second, ram_d_data, mmio_data;
    logic d_is_ram, d_is_ddr, i_is_ddr;
    logic ddr_d_pending, ddr_d_write;
    logic [31:0] ddr_d_addr, ddr_d_wdata;
    logic [3:0] ddr_d_strb;
    logic [63:0] mtime, mtimecmp;
    logic msip;
    logic tx_ready, tx_valid;
    logic uart_wait;
    logic mmio_pending;
    logic [31:0] mmio_addr, mmio_wdata;
    logic [3:0] mmio_strb;
    logic mmio_write;
    logic [7:0] uart_data;
    wire i_ext = DDR_ENABLE && ia[31:30] == 2'b10;
    wire d_ext = DDR_ENABLE && da[31:30] == 2'b10;
    wire mi_ready, md_ready, mi_valid, md_valid, md_error;
    wire [1:0] mi_error;
    wire [63:0] mi_data;
    wire [31:0] md_data;
    wire cpu_fence_i,cache_busy,cache_error;
    logic cache_flush;
    wire [31:0] cache_ihit,cache_imiss,cache_dhit,cache_dmiss,cache_wb,cache_ib_hit;
    wire mmio_fire = PIPELINED_MMIO ? mmio_pending :
        (dv && dr && !d_ext && da >= RAM_WORDS * 4);
    wire [31:0] peripheral_addr = PIPELINED_MMIO ? mmio_addr : da;
    wire [31:0] peripheral_data = PIPELINED_MMIO ? mmio_wdata : dd;
    wire [3:0] peripheral_strb = PIPELINED_MMIO ? mmio_strb : ds;
    wire peripheral_write = PIPELINED_MMIO ? mmio_write : dw;
    integer j;
    initial begin
        if (INIT_FILE != "") $readmemh(INIT_FILE, ram);
    end
    assign ir = i_ext ? mi_ready : (!irv || irr);
    // Accept the request before decoding a slow peripheral. A UART store
    // holds its response until TX accepts it; ready has no address/data path.
    // Ready depends only on registered queue/response state, never on the
    // newly generated address. DDR requests are decoupled by a one-entry slot.
    assign dr = (!drv || drr) && !uart_wait && !mmio_pending && !ddr_d_pending;
    assign idata = {ram_i_second, ram_i_data};
    // Capture the response source when accepting a request. The select is
    // settled before RAM data arrives and does not depend on DDR rsp_valid.
    assign drdata = DDR_ENABLE ?
        (({32{d_is_ram}} & ram_d_data) | ({32{d_is_ddr}} & md_data) |
         ({32{!d_is_ram && !d_is_ddr}} & mmio_data)) :
        (d_is_ram ? ram_d_data : mmio_data);
    assign tx_valid = uart_wait;
    generate if (DDR_ENABLE) begin : with_ddr
        wire lv,lr,lwrite,linstr,rv,rr,re;
        wire [29:0] la;
        wire [255:0] ld,rd;
        wire [31:0] ls;
        qinglong_extmem #(.I_BUFFER(CACHE_ENABLE)) extmem (
            .invalidate_i(cache_busy || cache_error),.i_buffer_hits(cache_ib_hit),
            .clk(clk), .rst(rst),
            .i_req_valid(iv && i_ext), .i_req_ready(mi_ready), .i_req_addr(ia),
            .i_rsp_valid(mi_valid), .i_rsp_ready(irr), .i_rsp_data(mi_data), .i_rsp_error(mi_error),
            .d_req_valid(ddr_d_pending), .d_req_ready(md_ready), .d_req_addr(ddr_d_addr),
            .d_req_wdata(ddr_d_wdata), .d_req_wstrb(ddr_d_strb), .d_req_write(ddr_d_write),
            .d_rsp_valid(md_valid), .d_rsp_ready(drr), .d_rsp_data(md_data), .d_rsp_error(md_error),
            .req_valid(lv), .req_ready(lr), .req_addr(la), .req_instruction(linstr),
            .req_write(lwrite), .req_wdata(ld), .req_wstrb(ls),
            .rsp_valid(rv), .rsp_ready(rr), .rsp_data(rd), .rsp_error(re)
        );
        if (CACHE_ENABLE) begin : cached
            qinglong_l1cache #(.SET_BITS(CACHE_SET_BITS)) l1 (
                .clk(clk),.rst(rst),.maintenance_req(cpu_fence_i || cache_flush),
                .maintenance_busy(cache_busy),.maintenance_error(cache_error),
                .req_valid(lv),.req_ready(lr),.req_addr(la),.req_instruction(linstr),
                .req_write(lwrite),.req_wdata(ld),.req_wstrb(ls),
                .rsp_valid(rv),.rsp_ready(rr),.rsp_data(rd),.rsp_error(re),
                .mem_req_valid(mem_req_valid),.mem_req_ready(mem_req_ready),.mem_req_addr(mem_req_addr),
                .mem_req_write(mem_req_write),.mem_req_wdata(mem_req_wdata),.mem_req_wstrb(mem_req_wstrb),
                .mem_rsp_valid(mem_rsp_valid),.mem_rsp_ready(mem_rsp_ready),.mem_rsp_data(mem_rsp_data),.mem_rsp_error(mem_rsp_error),
                .i_hits(cache_ihit),.i_misses(cache_imiss),.d_hits(cache_dhit),.d_misses(cache_dmiss),.writebacks(cache_wb));
        end else begin : uncached
            assign mem_req_valid=lv;assign lr=mem_req_ready;assign mem_req_addr=la;
            assign mem_req_write=lwrite;assign mem_req_wdata=ld;assign mem_req_wstrb=ls;
            assign rv=mem_rsp_valid;assign mem_rsp_ready=rr;assign rd=mem_rsp_data;assign re=mem_rsp_error;
            assign cache_busy=0;assign cache_error=0;assign cache_ihit=0;assign cache_imiss=0;
            assign cache_dhit=0;assign cache_dmiss=0;assign cache_wb=0;
        end
    end else begin : without_ddr
        assign cache_ib_hit=0;
        assign mi_ready = 0; assign md_ready = 0; assign mi_valid = 0; assign md_valid = 0;
        assign mi_error = 0; assign md_error = 0; assign mi_data = 0; assign md_data = 0;
        assign cache_busy=0;assign cache_error=0;assign cache_ihit=0;assign cache_imiss=0;
        assign cache_dhit=0;assign cache_dmiss=0;assign cache_wb=0;
        assign mem_req_valid = 0; assign mem_req_addr = 0; assign mem_req_write = 0;
        assign mem_req_wdata = 0; assign mem_req_wstrb = 0; assign mem_rsp_ready = 0;
    end endgenerate
    uart_tx #(.CLOCK_HZ(CLOCK_HZ)) tx_unit (
        .clk(clk), .rst(rst), .valid(tx_valid), .data(uart_data), .ready(tx_ready), .tx(uart_tx_o)
    );
    rv32_core #(.PREDICT_ENABLE(PREDICT_ENABLE), .BTB_BITS(BTB_BITS),
                .DUAL_ISSUE(DUAL_ISSUE), .LOAD_BYPASS(LOAD_BYPASS), .LOAD_WORD_ONLY(LOAD_WORD_ONLY), .EARLY_LSU(EARLY_LSU),
                .MULH_PIPELINE(MULH_PIPELINE)) cpu (
        .clk(clk), .rst(rst),
        .fence_i_o(cpu_fence_i), .i_req_valid(iv), .i_req_ready(ir), .i_req_addr(ia),
        .i_rsp_valid(irv || mi_valid), .i_rsp_ready(irr),
        .i_rsp_data(i_is_ddr ? mi_data : idata), .i_rsp_error(i_is_ddr ? mi_error : ire),
        .d_req_valid(dv), .d_req_ready(dr), .d_req_addr(da), .d_req_wdata(dd),
        .d_req_wstrb(ds), .d_req_write(dw),
        .d_rsp_valid(drv || md_valid), .d_rsp_ready(drr),
        .d_rsp_data(drdata), .d_rsp_error(d_is_ddr ? md_error : dre),
        .irq_software(msip), .irq_timer(mtime >= mtimecmp), .irq_external(irq_external),
        .retire_valid(retire_valid), .retire_pc(retire_pc), .retire_insn(retire_insn),
        .retire_data(retire_data), .retire_rd(retire_rd),
        .retire_second_valid(retire_second_valid), .retire_second_pc(retire_second_pc),
        .retire_second_insn(retire_second_insn), .retire_second_data(retire_second_data),
        .retire_second_rd(retire_second_rd), .dual_retire_count(dual_retire_count),
        .trap_valid(trap_valid), .trap_pc(trap_pc), .trap_cause(trap_cause),
        .trap_value(trap_value), .cycle_count(cycle_count), .instret_count(instret_count),
        .branch_count(branch_count), .mispredict_count(mispredict_count),
        .load_stall_count(load_stall_count)
    );
    // No reset on data or arrays: preserve synchronous block-RAM inference.
    // Software must FENCE.I after code writes; concurrent same-address port
    // read/write data is deliberately not relied on by the CPU/software.
    always_ff @(posedge clk) begin
        if (iv && ir && ia < RAM_WORDS * 4)
            ram_i_data <= ram[ia[AW+1:2]];
        if (iv && ir && ia < RAM_WORDS * 4 - 4)
            ram_i_second <= ram[ia[AW+1:2] + 1'b1];
        // Speculate reads while the response slot is free. Address range and
        // request-valid stay on write enables only; held responses stay stable.
        if (!drv || drr) ram_d_data <= ram[da[AW+1:2]];
        if (dv && dr && da < RAM_WORDS * 4) begin
            if (dw)
                for (j = 0; j < 4; j = j + 1)
                    if (ds[j]) ram[da[AW+1:2]][8*j +: 8] <= dd[8*j +: 8];
        end
    end
    always_ff @(posedge clk) begin
        if (rst) begin
            irv <= 0; ire <= 0; drv <= 0; dre <= 0; d_is_ram <= 0; d_is_ddr <= 0; i_is_ddr <= 0;
            mmio_data <= 0; gpio <= 0; done <= 0; exit_code <= 0;
            console_valid <= 0; console_data <= 0; mtime <= 0; mtimecmp <= '1; msip <= 0;
            uart_wait <= 0; uart_data <= 0;
            ddr_d_pending <= 0; ddr_d_write <= 0; ddr_d_addr <= 0; ddr_d_wdata <= 0; ddr_d_strb <= 0;
            cache_flush <= 0;
            mmio_pending <= 0; mmio_addr <= 0; mmio_wdata <= 0; mmio_strb <= 0; mmio_write <= 0;
        end else begin
            cache_flush <= 0;
            mtime <= mtime + 64'd1;
            if (ddr_d_pending && md_ready) ddr_d_pending <= 0;
            if (DDR_ENABLE && dv && dr && d_ext) begin
                ddr_d_pending <= 1; ddr_d_addr <= da;
                ddr_d_wdata <= dd; ddr_d_strb <= ds; ddr_d_write <= dw;
            end
            console_valid <= 0;
            if (iv && ir) i_is_ddr <= i_ext;
            if (dv && dr) begin
                d_is_ram <= da < RAM_WORDS * 4;
                d_is_ddr <= d_ext;
            end
            if (irr) irv <= 0;
            if (iv && ir && !i_ext) begin
                irv <= 1;
                ire <= {ia >= RAM_WORDS * 4 - 4, ia >= RAM_WORDS * 4};
            end
            if (drr) drv <= 0;
            if (uart_wait && tx_ready) begin
                uart_wait <= 0; drv <= 1;
                console_valid <= 1; console_data <= uart_data;
            end
            if (mmio_pending) mmio_pending <= 0;
            if (dv && dr && !d_ext) begin
                drv <= 1; dre <= 0; mmio_data <= 0;
                if (PIPELINED_MMIO && da >= RAM_WORDS * 4) begin
                    // Accept the address before decoding peripheral registers.
                    // The CPU waits for the response; TCM retains its fast path.
                    mmio_pending <= 1; drv <= 0;
                    mmio_addr <= da; mmio_wdata <= dd;
                    mmio_strb <= ds; mmio_write <= dw;
                end
            end
            if (mmio_fire) begin
                drv <= 1; dre <= 0; mmio_data <= 0;
                case (peripheral_addr)
                    32'h10000000: begin
                        mmio_data <= {31'b0, (tx_ready || SIM_FAST_UART)};
                        if (peripheral_write && peripheral_strb[0]) begin
                            if (SIM_FAST_UART) begin
                                console_valid <= 1; console_data <= peripheral_data[7:0];
                            end else begin
                                uart_wait <= 1; uart_data <= peripheral_data[7:0]; drv <= 0;
                            end
                        end
                    end
                    32'h10000004: if (peripheral_write && peripheral_strb == 15) begin done <= 1; exit_code <= peripheral_data; end
                    32'h10000008: begin
                        mmio_data <= gpio;
                        if (peripheral_write) for (j = 0; j < 4; j = j + 1)
                            if (peripheral_strb[j]) gpio[8*j +: 8] <= peripheral_data[8*j +: 8];
                    end
                    32'h10000100: mmio_data <= mtime[31:0];
                    32'h10000104: mmio_data <= mtime[63:32];
                    32'h10000108: begin
                        mmio_data <= mtimecmp[31:0];
                        if (peripheral_write && peripheral_strb == 15) mtimecmp[31:0] <= peripheral_data;
                    end
                    32'h1000010c: begin
                        mmio_data <= mtimecmp[63:32];
                        if (peripheral_write && peripheral_strb == 15) mtimecmp[63:32] <= peripheral_data;
                    end
                    32'h10000110: begin
                        mmio_data <= {31'b0, msip};
                        if (peripheral_write && peripheral_strb[0]) msip <= peripheral_data[0];
                    end
                    32'h10000200: begin
                        // bit 0: calibrated and usable, bit 1: sticky fault,
                        // bit 2: external-memory interface compiled in.
                        if (DDR_ENABLE) begin
                            mmio_data <= {26'b0, CACHE_ENABLE, cache_error, (cache_busy || cache_flush), 1'b1, ddr_fault, ddr_ready};
                            if (peripheral_write) dre <= 1;
                        end else dre <= 1;
                    end
                    32'h10000204: begin
                        if(CACHE_ENABLE && DDR_ENABLE) begin
                            mmio_data <= {30'b0,cache_error,(cache_busy || cache_flush)};
                            if(peripheral_write && peripheral_strb[0] && peripheral_data[0]) cache_flush<=1;
                        end else dre<=1;
                    end
                    32'h10000210: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_ihit;else dre<=1;
                    32'h10000214: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_imiss;else dre<=1;
                    32'h10000218: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_dhit;else dre<=1;
                    32'h1000021c: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_dmiss;else dre<=1;
                    32'h10000220: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_wb;else dre<=1;
                    32'h10000224: if(CACHE_ENABLE && DDR_ENABLE) mmio_data<=cache_ib_hit;else dre<=1;
                    default: dre <= 1;
                endcase
            end
        end
    end
endmodule
