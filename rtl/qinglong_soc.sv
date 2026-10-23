// Minimal CPU integration platform. Unified dual-port TCM, no DDR/cache yet.
module qinglong_soc #(
    parameter integer RAM_WORDS = 16384,
    parameter INIT_FILE = "build/firmware.hex",
    parameter integer CLOCK_HZ = 50000000,
    parameter bit SIM_FAST_UART = 0,
    parameter bit PREDICT_ENABLE = 1,
    parameter integer BTB_BITS = 8,
    parameter bit DUAL_ISSUE = 1,
    parameter bit LOAD_BYPASS = 1,
    parameter bit EARLY_LSU = 1
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
    output logic [31:0] branch_count, mispredict_count, load_stall_count
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
    logic d_is_ram;
    logic [63:0] mtime, mtimecmp;
    logic msip;
    logic tx_ready, tx_valid;
    logic uart_wait;
    logic [7:0] uart_data;
    integer j;
    initial begin
        if (INIT_FILE != "") $readmemh(INIT_FILE, ram);
    end
    assign ir = !irv || irr;
    // Accept the request before decoding a slow peripheral. A UART store
    // holds its response until TX accepts it; ready has no address/data path.
    assign dr = (!drv || drr) && !uart_wait;
    assign idata = {ram_i_second, ram_i_data};
    assign drdata = d_is_ram ? ram_d_data : mmio_data;
    assign tx_valid = uart_wait;
    uart_tx #(.CLOCK_HZ(CLOCK_HZ)) tx_unit (
        .clk(clk), .rst(rst), .valid(tx_valid), .data(uart_data), .ready(tx_ready), .tx(uart_tx_o)
    );
    rv32_core #(.PREDICT_ENABLE(PREDICT_ENABLE), .BTB_BITS(BTB_BITS),
                .DUAL_ISSUE(DUAL_ISSUE), .LOAD_BYPASS(LOAD_BYPASS), .EARLY_LSU(EARLY_LSU)) cpu (
        .clk(clk), .rst(rst),
        .i_req_valid(iv), .i_req_ready(ir), .i_req_addr(ia),
        .i_rsp_valid(irv), .i_rsp_ready(irr), .i_rsp_data(idata), .i_rsp_error(ire),
        .d_req_valid(dv), .d_req_ready(dr), .d_req_addr(da), .d_req_wdata(dd),
        .d_req_wstrb(ds), .d_req_write(dw),
        .d_rsp_valid(drv), .d_rsp_ready(drr), .d_rsp_data(drdata), .d_rsp_error(dre),
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
        if (dv && dr && da < RAM_WORDS * 4) begin
            ram_d_data <= ram[da[AW+1:2]];
            if (dw)
                for (j = 0; j < 4; j = j + 1)
                    if (ds[j]) ram[da[AW+1:2]][8*j +: 8] <= dd[8*j +: 8];
        end
    end
    always_ff @(posedge clk) begin
        if (rst) begin
            irv <= 0; ire <= 0; drv <= 0; dre <= 0; d_is_ram <= 0;
            mmio_data <= 0; gpio <= 0; done <= 0; exit_code <= 0;
            console_valid <= 0; console_data <= 0; mtime <= 0; mtimecmp <= '1; msip <= 0;
            uart_wait <= 0; uart_data <= 0;
        end else begin
            mtime <= mtime + 64'd1;
            console_valid <= 0;
            if (irr) irv <= 0;
            if (iv && ir) begin
                irv <= 1;
                ire <= {ia >= RAM_WORDS * 4 - 4, ia >= RAM_WORDS * 4};
            end
            if (drr) drv <= 0;
            if (uart_wait && tx_ready) begin
                uart_wait <= 0; drv <= 1;
                console_valid <= 1; console_data <= uart_data;
            end
            if (dv && dr) begin
                drv <= 1; dre <= 0; d_is_ram <= da < RAM_WORDS * 4;
                mmio_data <= 0;
                if (da >= RAM_WORDS * 4) begin
                    case (da)
                        32'h10000000: begin
                            mmio_data <= {31'b0, (tx_ready || SIM_FAST_UART)};
                            if (dw && ds[0]) begin
                                if (SIM_FAST_UART) begin
                                    console_valid <= 1; console_data <= dd[7:0];
                                end else begin
                                    uart_wait <= 1; uart_data <= dd[7:0]; drv <= 0;
                                end
                            end
                        end
                        32'h10000004: if (dw && ds == 15) begin done <= 1; exit_code <= dd; end
                        32'h10000008: begin
                            mmio_data <= gpio;
                            if (dw) for (j = 0; j < 4; j = j + 1)
                                if (ds[j]) gpio[8*j +: 8] <= dd[8*j +: 8];
                        end
                        32'h10000100: mmio_data <= mtime[31:0];
                        32'h10000104: mmio_data <= mtime[63:32];
                        32'h10000108: begin
                            mmio_data <= mtimecmp[31:0];
                            if (dw && ds == 15) mtimecmp[31:0] <= dd;
                        end
                        32'h1000010c: begin
                            mmio_data <= mtimecmp[63:32];
                            if (dw && ds == 15) mtimecmp[63:32] <= dd;
                        end
                        32'h10000110: begin
                            mmio_data <= {31'b0, msip};
                            if (dw && ds[0]) msip <= dd[0];
                        end
                        default: dre <= 1;
                    endcase
                end
            end
        end
    end
endmodule
