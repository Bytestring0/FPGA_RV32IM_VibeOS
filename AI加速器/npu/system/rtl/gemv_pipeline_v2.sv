// Wider opt-in W4A8 array; original gemv_pipeline16 remains unchanged.
// Registered products, registered balanced sum, elastic tagged group accumulator.
module gemv_pipeline_v2 #(
    parameter integer LANES = 32
) (
    input wire clk, rst_n,
    input wire in_valid, output wire in_ready,
    input wire [LANES*4-1:0] weight_data,
    input wire [LANES*8-1:0] activation_data,
    input wire group_first, group_last,
    input wire [15:0] row_id, group_id,
    output wire out_valid, input wire out_ready,
    output wire signed [31:0] partial_sum,
    output wire [15:0] out_row_id, out_group_id
);
    localparam integer SUM_WIDTH = 12+$clog2(LANES);
    reg product_valid, sum_valid;
    reg product_first, product_last, sum_first, sum_last;
    reg [15:0] product_row, product_group, sum_row, sum_group;
    reg signed [31:0] sum_hold;
    reg signed [11:0] product_hold [0:LANES-1];
    wire accumulator_ready;
    wire sum_ready = !sum_valid || accumulator_ready;
    wire product_ready = !product_valid || sum_ready;
    assign in_ready = rst_n && product_ready;
    genvar lane;
    generate
        for (lane=0;lane<LANES;lane=lane+1) begin : macs
            wire signed [11:0] product;
            mac_lane mac(.weight(weight_data[lane*4 +:4]),
                         .activation(activation_data[lane*8 +:8]),.product(product));
            always @(posedge clk)
                if(product_ready && in_valid) product_hold[lane]<=product;
        end
        for(lane=1;lane<2*LANES;lane=lane+1) begin : reduction
            wire signed [SUM_WIDTH-1:0] sum;
            if(lane>=LANES) begin : leaf
                assign sum={{(SUM_WIDTH-12){product_hold[lane-LANES][11]}},product_hold[lane-LANES]};
            end else begin : branch
                assign sum=reduction[2*lane].sum+reduction[2*lane+1].sum;
            end
        end
    endgenerate
    wire signed [SUM_WIDTH-1:0] reduced_sum=reduction[1].sum;
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin product_valid<=0;sum_valid<=0;end
        else begin
            if(product_ready) product_valid<=in_valid;
            if(sum_ready) sum_valid<=product_valid;
        end
    end
    always @(posedge clk) begin
        if(product_ready && in_valid) begin
            product_first<=group_first;product_last<=group_last;
            product_row<=row_id;product_group<=group_id;
        end
        if(sum_ready && product_valid) begin
            sum_hold<={{(32-SUM_WIDTH){reduced_sum[SUM_WIDTH-1]}},reduced_sum};
            sum_first<=product_first;sum_last<=product_last;
            sum_row<=product_row;sum_group<=product_group;
        end
    end
    group_acc group_acc_inst(
        .clk(clk),.rst_n(rst_n),.in_valid(sum_valid),.in_ready(accumulator_ready),
        .beat_sum(sum_hold),.group_first(sum_first),.group_last(sum_last),
        .row_id(sum_row),.group_id(sum_group),.out_valid(out_valid),.out_ready(out_ready),
        .partial_sum(partial_sum),.out_row_id(out_row_id),.out_group_id(out_group_id));
endmodule
