`timescale 1ns/1ps
// Functional post-synthesis smoke with the locally installed vendor models.
// No SDF: this checks mapped logic/RAM initialization, not timing simulation.
module gate_smoke_tb;
    reg clk = 0, reset_n = 0;
    wire tx;
    wire [7:0] led;
    integer index, bit_index;
    reg [7:0] received;
    string expected = "QingLong RV32IM boot OK, hash=f60ac131\n";
    localparam integer DIVISOR = 27000000 / 115200;
    GTP_GRS GRS_INST (.GRS_N(reset_n));
    qinglong_fpga_top dut (.clk(clk), .reset_n(reset_n), .uart_txd(tx), .led(led));
    always #18.518 clk = ~clk;
    initial begin
        repeat (10) @(negedge clk);
        reset_n = 1;
        repeat (250000) @(negedge clk);
        $fatal(1, "GATE SMOKE timeout, LED=%02x", led);
    end
    initial begin
        wait(reset_n);
        for (index = 0; index < expected.len(); index = index+1) begin
            @(negedge tx);
            repeat (DIVISOR + DIVISOR/2) @(posedge clk);
            for (bit_index = 0; bit_index < 8; bit_index = bit_index+1) begin
                received[bit_index] = tx;
                repeat (DIVISOR) @(posedge clk);
            end
            if (tx !== 1'b1 || received !== expected[index])
                $fatal(1, "GATE UART mismatch index=%0d got=%02x expected=%02x stop=%b", index, received, expected[index], tx);
        end
        wait (led === 8'h01);
        $display("GATE SMOKE PASS: synthesized CPU, initialized TCM, multiply, memory and complete UART text");
        $finish;
    end
endmodule
