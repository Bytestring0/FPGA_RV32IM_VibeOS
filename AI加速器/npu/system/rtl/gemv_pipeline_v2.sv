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
    localparam integer MID_NODES=LANES/8;
    reg product_valid, middle_valid, sum_valid;
    reg middle_first,middle_last;
    reg [15:0] middle_row,middle_group;
    reg signed [SUM_WIDTH-1:0] middle_sum[0:MID_NODES-1];
    reg product_first, product_last, sum_first, sum_last;
    reg [15:0] product_row, product_group, sum_row, sum_group;
    reg signed [31:0] sum_hold;
    reg signed [11:0] product_hold [0:LANES-1];
    wire accumulator_ready;
    wire sum_ready = !sum_valid || accumulator_ready;
    wire middle_ready = !middle_valid || sum_ready;
    wire product_ready = !product_valid || middle_ready;
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
            end else if(lane>=MID_NODES&&lane<2*MID_NODES)begin: registered_cut
                assign sum=middle_sum[lane-MID_NODES];
                always @(posedge clk)if(middle_ready&&product_valid)
                    middle_sum[lane-MID_NODES]<=reduction[2*lane].sum+reduction[2*lane+1].sum;
            end else begin : branch
                assign sum=reduction[2*lane].sum+reduction[2*lane+1].sum;
            end
        end
    endgenerate
    wire signed [SUM_WIDTH-1:0] reduced_sum=reduction[1].sum;
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin product_valid<=0;middle_valid<=0;sum_valid<=0;end
        else begin
            if(product_ready) product_valid<=in_valid;
            if(middle_ready)middle_valid<=product_valid;
            if(sum_ready) sum_valid<=middle_valid;
        end
    end
    always @(posedge clk) begin
        if(product_ready && in_valid) begin
            product_first<=group_first;product_last<=group_last;
            product_row<=row_id;product_group<=group_id;
        end
        if(middle_ready&&product_valid)begin
            middle_first<=product_first;middle_last<=product_last;
            middle_row<=product_row;middle_group<=product_group;
        end
        if(sum_ready && middle_valid) begin
            sum_hold<={{(32-SUM_WIDTH){reduced_sum[SUM_WIDTH-1]}},reduced_sum};
            sum_first<=middle_first;sum_last<=middle_last;
            sum_row<=middle_row;sum_group<=middle_group;
        end
    end
    group_acc group_acc_inst(
        .clk(clk),.rst_n(rst_n),.in_valid(sum_valid),.in_ready(accumulator_ready),
        .beat_sum(sum_hold),.group_first(sum_first),.group_last(sum_last),
        .row_id(sum_row),.group_id(sum_group),.out_valid(out_valid),.out_ready(out_ready),
        .partial_sum(partial_sum),.out_row_id(out_row_id),.out_group_id(out_group_id));
endmodule
