// MES2L676-100HP / PG2L100H-6IFBG676; pins documented in board_pins.fdc.
module qinglong_board_system #(
    parameter integer CLOCK_MHZ = 27
) (
    input wire clk_27m, reset_n,
    output wire uart_txd,
    output wire [7:0] led
);
    wire cpu_clk, pll_locked;
    qinglong_board_clock #(.CLOCK_MHZ(CLOCK_MHZ)) clocking_unit (
        .clk_27m(clk_27m), .cpu_clk(cpu_clk), .locked(pll_locked)
    );
    // The CPU wrapper asserts reset asynchronously on button/lock loss and
    // releases it only after three edges in the selected CPU clock domain.
    qinglong_fpga_top #(.CLOCK_HZ(CLOCK_MHZ * 1000000)) system (
        .clk(cpu_clk), .reset_n(reset_n && pll_locked), .uart_txd(uart_txd), .led(led)
    );
endmodule
