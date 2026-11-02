// V1 baseline: combinational lane products/reduction, registered group sum.
// Legal input groups must contain exactly 128/LANES accepted beats.
module gemv_core #(
    parameter integer LANES = 32
) (
    input wire clk,
    input wire rst_n,
    input wire in_valid,
    output wire in_ready,
    input wire [LANES*4-1:0] weight_data,
    input wire [LANES*8-1:0] activation_data,
    input wire group_first,
    input wire group_last,
    input wire [15:0] row_id,
    input wire [15:0] group_id,
    output wire out_valid,
    input wire out_ready,
    output wire signed [31:0] partial_sum,
    output wire [15:0] out_row_id,
    output wire [15:0] out_group_id
);
    wire signed [11:0] products [0:LANES-1];
    // mac_lane interprets each extracted nibble as signed and extends it.
    genvar lane;
    generate
        for (lane = 0; lane < LANES; lane = lane+1) begin : macs
            mac_lane mac (
                .weight(weight_data[lane*4 +: 4]),
                .activation(activation_data[lane*8 +: 8]),
                .product(products[lane])
            );
        end
        // Individual node wires also avoid false array-loop reports in older lint.
        for (lane = 1; lane < 2*LANES; lane = lane+1) begin : reduction
            wire signed [31:0] sum;
            if (lane >= LANES) begin : leaf
                assign sum = {{20{products[lane-LANES][11]}}, products[lane-LANES]};
            end else begin : branch
                assign sum = reduction[2*lane].sum + reduction[2*lane+1].sum;
            end
        end
    endgenerate
    group_acc group_acc_inst (
        .clk(clk), .rst_n(rst_n), .in_valid(in_valid), .in_ready(in_ready),
        .beat_sum(reduction[1].sum), .group_first(group_first), .group_last(group_last),
        .row_id(row_id), .group_id(group_id), .out_valid(out_valid),
        .out_ready(out_ready), .partial_sum(partial_sum),
        .out_row_id(out_row_id), .out_group_id(out_group_id)
    );
endmodule
