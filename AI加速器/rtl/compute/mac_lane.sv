// Combinational product. Group accumulation belongs to group_acc.
module mac_lane (
    input  wire signed [3:0] weight,
    input  wire signed [7:0] activation,
    output wire signed [11:0] product
);
    wire signed [11:0] weight_ext = {{8{weight[3]}}, weight};
    wire signed [11:0] activation_ext = {{4{activation[7]}}, activation};
    assign product = weight_ext * activation_ext;
endmodule
