// Delta Q/K/V register files: fixed eight-lane stripe, one write port per bank.
// Q,K scalar read is asynchronous and V has one aligned eight-word read.
// No reset: callers load every active word before use. Conv's eight-word line
// buffer is deliberately separate, avoiding a second write address/port on Q.
module npu_delta_vectors #(
 parameter bit DELTA_ENABLE=1, parameter bit CONV_ENABLE=1
)(
 input wire clk,
 input wire q_we,k_we,v_we,conv_we,
 input wire [3:0] write_row,
 input wire [255:0] write_data,
 input wire [6:0] scalar_index,
 input wire [3:0] v_read_row,
 input wire [2:0] conv_index,
 output wire signed [31:0] q_scalar,k_scalar,
 output wire [255:0] v_vector,
 output wire signed [31:0] conv_scalar
);
 wire [255:0] q_candidates,k_candidates,conv_candidates;
 genvar lane;
 generate for(lane=0;lane<8;lane=lane+1)begin: banks
  if(DELTA_ENABLE)begin: delta
   reg [31:0] q_bank[0:15],k_bank[0:15],v_bank[0:15];
   always @(posedge clk)begin
    if(q_we)q_bank[write_row]<=write_data[lane*32+:32];
    if(k_we)k_bank[write_row]<=write_data[lane*32+:32];
    if(v_we)v_bank[write_row]<=write_data[lane*32+:32];
   end
   assign q_candidates[lane*32+:32]=q_bank[scalar_index[6:3]];
   assign k_candidates[lane*32+:32]=k_bank[scalar_index[6:3]];
   assign v_vector[lane*32+:32]=v_bank[v_read_row];
  end else begin: no_delta
   assign q_candidates[lane*32+:32]=32'd0;
   assign k_candidates[lane*32+:32]=32'd0;
   assign v_vector[lane*32+:32]=32'd0;
  end
  if(CONV_ENABLE)begin: conv
   reg [31:0] input_word;
   always @(posedge clk)if(conv_we)input_word<=write_data[lane*32+:32];
   assign conv_candidates[lane*32+:32]=input_word;
  end else begin: no_conv
   assign conv_candidates[lane*32+:32]=32'd0;
  end
 end endgenerate
 assign q_scalar=q_candidates[scalar_index[2:0]*32+:32];
 assign k_scalar=k_candidates[scalar_index[2:0]*32+:32];
 assign conv_scalar=conv_candidates[conv_index*32+:32];
endmodule
