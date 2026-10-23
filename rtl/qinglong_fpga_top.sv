// Raw-clock bring-up wrapper. clk is 27 MHz on the documented 100HP board.
// To raise CPU frequency, replace clk with a locked PLL output and adjust
// CLOCK_HZ + timing constraints together; never divide/gate clocks in fabric.
module qinglong_fpga_top #(
    parameter integer CLOCK_HZ = 27000000
) (
    input logic clk,
    input logic reset_n,
    output logic uart_txd,
    output logic [7:0] led
);
    logic [2:0] reset_sync = 3'b111;
    logic [31:0] gpio, exit_code;
    logic done;
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) reset_sync <= 3'b111;
        else reset_sync <= {reset_sync[1:0], 1'b0};
    end
    assign led = done ? (exit_code == 0 ? 8'h01 : 8'h80) : gpio[7:0];
    qinglong_soc #(.CLOCK_HZ(CLOCK_HZ), .INIT_FILE("firmware.hex")) soc (
        .clk(clk), .rst(reset_sync[2]), .irq_external(1'b0),
        .uart_tx_o(uart_txd), .gpio(gpio), .done(done), .exit_code(exit_code),
        .console_valid(), .console_data(), .retire_valid(), .retire_pc(),
        .retire_insn(), .retire_data(), .retire_rd(), .trap_valid(), .trap_pc(),
        .retire_second_valid(), .retire_second_pc(), .retire_second_insn(),
        .retire_second_data(), .retire_second_rd(), .dual_retire_count(),
        .trap_cause(), .trap_value(), .cycle_count(), .instret_count(),
        .branch_count(), .mispredict_count(), .load_stall_count()
    );
endmodule
