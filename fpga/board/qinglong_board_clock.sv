// Logos2 GPLL configuration; primitive declaration/model is supplied by PDS.
// UG041001: VCO = Fin * MDIV * FDIV / IDIV; Fout = VCO / ODIV.
// 27 -> 108 MHz: 27 * 4 * 8 / 1 = 864 MHz VCO, divided by 8.
// 27 -> 81 MHz: 27 * 3 * 8 / 1 = 648 MHz VCO, divided by 8.
// 27 -> 96 MHz: 27 * 16 * 2 / 1 = 864 MHz VCO, divided by 9.
// 27 -> 102 MHz: 27 * 17 * 2 / 1 = 918 MHz VCO, divided by 9.
module qinglong_board_clock #(
    parameter integer CLOCK_MHZ = 108
) (
    input wire clk_27m,
    output wire cpu_clk, locked
);
    generate if (CLOCK_MHZ == 27) begin : bypass
        assign cpu_clk = clk_27m;
        assign locked = 1'b1;
    end else begin : pll_mode
        wire pll_clk;
        GTP_GPLL #(
            .CLKIN_FREQ(27.0), .STATIC_RATIOI(1),
            .STATIC_RATIOM((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? CLOCK_MHZ / 6 : CLOCK_MHZ / 27),
            .STATIC_RATIOF((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 2.0 : 8.0),
            .STATIC_RATIO0((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 9.0 : 8.0),
            .STATIC_DUTYF((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 2 : 8),
            .STATIC_DUTY0((CLOCK_MHZ == 96 || CLOCK_MHZ == 102) ? 9 : 8),
            .INTERNAL_FB("CLKOUTF"), .EXTERNAL_FB("DISABLE"),
            .LOCK_MODE(1'b0)
        ) pll (
            .CLKIN1(clk_27m), .CLKIN2(1'b0), .CLKIN_SEL(1'b0),
            // Keep the PLL running across CPU reset-button presses. Its
            // configuration reset is provided by FPGA configuration itself.
            .CLKFB(1'b0), .RST(1'b0), .PLL_PWD(1'b0),
            .CLKOUT0(pll_clk), .LOCK(locked),
            .DPS_CLK(1'b0), .DPS_EN(1'b0), .DPS_DIR(1'b0),
            .CLKOUT0_SYN(1'b0), .CLKOUT1_SYN(1'b0),
            .CLKOUT2_SYN(1'b0), .CLKOUT3_SYN(1'b0),
            .CLKOUT4_SYN(1'b0), .CLKOUT5_SYN(1'b0),
            .CLKOUT6_SYN(1'b0), .CLKOUTF_SYN(1'b0),
            .APB_CLK(1'b0), .APB_RST_N(1'b0), .APB_ADDR(5'b0),
            .APB_SEL(1'b0), .APB_EN(1'b0), .APB_WRITE(1'b0), .APB_WDATA(16'b0)
        );
        GTP_CLKBUFG cpu_clock_buffer (.CLKIN(pll_clk), .CLKOUT(cpu_clk));
    end endgenerate
endmodule
