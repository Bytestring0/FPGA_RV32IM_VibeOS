// One pending output register. Can accept a new beat while retiring its output.
module group_acc (
    input wire clk,
    input wire rst_n,
    input wire in_valid,
    output wire in_ready,
    input wire signed [31:0] beat_sum,
    input wire group_first,
    input wire group_last,
    input wire [15:0] row_id,
    input wire [15:0] group_id,
    output reg out_valid,
    input wire out_ready,
    output reg signed [31:0] partial_sum,
    output reg [15:0] out_row_id,
    output reg [15:0] out_group_id
);
    reg signed [31:0] accumulator;
    wire signed [31:0] next_sum = group_first ? beat_sum : accumulator + beat_sum;
    assign in_ready = rst_n && (!out_valid || out_ready);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            accumulator <= 0;
            out_valid <= 0;
            partial_sum <= 0;
            out_row_id <= 0;
            out_group_id <= 0;
        end else begin
            if (out_valid && out_ready)
                out_valid <= 0;
            if (in_valid && in_ready) begin
                if (group_last) begin
                    accumulator <= 0;
                    out_valid <= 1;
                    partial_sum <= next_sum;
                    out_row_id <= row_id;
                    out_group_id <= group_id;
                end else begin
                    accumulator <= next_sum;
                end
            end
        end
    end
endmodule
