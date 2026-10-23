module uart_tx #(
    parameter integer CLOCK_HZ = 50000000,
    parameter integer BAUD = 115200
) (
    input logic clk, rst, valid,
    input logic [7:0] data,
    output logic ready, tx
);
    localparam integer DIVISOR = CLOCK_HZ / BAUD;
    localparam integer CW = $clog2(DIVISOR + 1);
    localparam logic [31:0] RELOAD = DIVISOR - 1;
    logic [CW-1:0] counter;
    logic [9:0] shift;
    logic [3:0] bits_left;
    assign ready = bits_left == 0;
    assign tx = ready ? 1'b1 : shift[0];
    always_ff @(posedge clk) begin
        if (rst) begin counter <= 0; shift <= '1; bits_left <= 0; end
        else if (valid && ready) begin
            shift <= {1'b1, data, 1'b0};
            bits_left <= 10; counter <= RELOAD[CW-1:0];
        end else if (!ready) begin
            if (counter == 0) begin
                shift <= {1'b1, shift[9:1]};
                bits_left <= bits_left - 4'd1;
                counter <= RELOAD[CW-1:0];
            end else counter <= counter - 1'b1;
        end
    end
endmodule
