// Radix-2 restoring divider, no combinational "/" datapath.
// start is sampled when idle. done pulses once; result remains stable.
module rv32_divider (
    input logic clk, rst, cancel, start,
    input logic [31:0] a, b,
    input logic [2:0] funct3,
    output logic busy, done,
    output logic [31:0] result
);
    logic [31:0] quotient, divisor;
    logic [32:0] remainder, trial, difference;
    logic [5:0] count;
    logic negate_q, negate_r, want_rem, prepare, negate_divisor;
    logic [31:0] q_next, r_next;
    wire signed_op = !funct3[0];
    always_comb begin
        trial = {remainder[31:0], quotient[31]};
        difference = trial - {1'b0, divisor};
        q_next = {quotient[30:0], !difference[32]};
        r_next = difference[32] ? trial[31:0] : difference[31:0];
    end
    always_ff @(posedge clk) begin
        if (rst || cancel) begin
            busy <= 0; done <= 0; result <= 0;
            quotient <= 0; divisor <= 0; remainder <= 0; count <= 0;
            negate_q <= 0; negate_r <= 0; want_rem <= 0;
            prepare <= 0; negate_divisor <= 0;
        end else begin
            done <= 0;
            // Idle operand capture is unconditional; start only controls the
            // transaction state. Keep its late hazard/exception logic out of
            // the wide operand registers' clock-enable chain.
            if (!busy) begin quotient <= a; divisor <= b; end
            if (start && !busy) begin
                    // Capture raw operands before the absolute-value adders.
                    // Unsigned and nonnegative signed divides start directly.
                    // Zero/overflow operands follow the same iteration path,
                    // removing wide operand comparisons from the input enable.
                    prepare <= signed_op && (a[31] || b[31]);
                    negate_divisor <= signed_op && b[31];
                    remainder <= 0; count <= 0; busy <= 1;
                    negate_q <= signed_op && (a[31] ^ b[31]);
                    negate_r <= signed_op && a[31]; want_rem <= funct3[1];
            end else if (busy) begin
                if (prepare) begin
                    quotient <= negate_r ? -quotient : quotient;
                    divisor <= negate_divisor ? -divisor : divisor;
                    prepare <= 0;
                end else begin
                quotient <= q_next; remainder <= {1'b0, r_next};
                count <= count + 6'd1;
                if (count == 31) begin
                    busy <= 0; done <= 1;
                    // Restoring division naturally preserves the dividend as
                    // remainder for zero divisors. DIV[U] must return all ones,
                    // including negative signed dividends. Signed overflow
                    // naturally wraps to INT_MIN with remainder zero.
                    result <= want_rem ? (negate_r ? -r_next : r_next) :
                              divisor == 0 ? 32'hffffffff :
                              (negate_q ? -q_next : q_next);
                end
                end
            end
        end
    end
endmodule
