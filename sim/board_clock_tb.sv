`timescale 1ns/1ps
module board_clock_tb;
    parameter integer CLOCK_MHZ = 108;
    reg clk = 0, reference_running = 1;
    wire cpu_clk, locked;
    realtime last_edge, period_ns;
    integer edges = 0;
    GTP_GRS global_reset (.GRS_N(1'b1));
    always #18.5185185 clk = reference_running ? ~clk : 1'b0;
    ag_board_clock #(.CLOCK_MHZ(CLOCK_MHZ)) dut (
        .clk_27m(clk), .cpu_clk(cpu_clk), .locked(locked)
    );
    initial begin
        wait(locked);
        repeat(10) @(posedge cpu_clk);
        last_edge = $realtime;
        repeat(100) begin
            @(posedge cpu_clk);
            period_ns = $realtime - last_edge;
            if (period_ns < 1000.0/CLOCK_MHZ-0.01 || period_ns > 1000.0/CLOCK_MHZ+0.01)
                $fatal(1, "Unexpected PLL period: %f ns", period_ns);
            last_edge = $realtime;
            edges = edges + 1;
        end
        @(negedge clk);
        reference_running = 0;
        #2000;
        if (locked) $fatal(1, "PLL lock did not clear after reference loss");
        reference_running = 1;
        wait(locked);
        repeat(10) @(posedge cpu_clk);
        $display("PASS vendor PLL: 27 -> %0d MHz, 100 measured periods, reference loss/relock", CLOCK_MHZ);
        $finish;
    end
    initial begin #2000000; $fatal(1, "PLL did not lock"); end
endmodule
