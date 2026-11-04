// Exact opcode19 fused raw-W4 reduction, II=1 (one ordered group per clock).
// Shared 11x32 coefficient and signed32x32 term multipliers; three 3-digit
// radix127 stages; fixed RNE14 and ordered add64. No 128-bit arithmetic.
// Legal positive-finite FP16 and maximum<=0x80000000 prove numerator<2^61.
module npu_w4_fused_reduce (
 input wire clk, rst, clear,
 input wire in_valid, output wire in_ready,
 input wire signed [31:0] partial_sum,
 input wire [15:0] scale_bits,
 input wire [31:0] maximum,
 input wire first_group, last_group,
 output reg term_done, row_done, invalid_scale,
 output reg signed [31:0] row_result,
 output reg [31:0] numeric_flags, saturation_count, underflow_count, invalid_count
);
 reg [9:0] valid_pipe,last_pipe,bad_pipe;
 reg [8:0] first_pipe;
 reg fault_seen;
 reg signed [31:0] partial_pipe[0:6];
 reg [42:0] coefficient_product;
 reg [4:0] exponent;
 reg [62:0] numerator[1:4],digits[2:4],quotient[2:4];
 reg [3:0] shift_right[1:4];
 reg [6:0] modulo[2:4];
 reg [62:0] truncated,remainder,denominator;
 reg nonzero_numerator;
 reg signed [31:0] coefficient;
 reg coefficient_sat,coefficient_under;
 reg [1:0] sat_pipe[7:10],under_pipe[7:10];
 reg signed [63:0] term_product,rounded_term,accumulator,sum_hold;
 wire [62:0] shifted_quotient=quotient[4]>>shift_right[4];
 wire [62:0] rounded_coefficient=truncated+
  {62'd0,(remainder>denominator-remainder ||
   (remainder==denominator-remainder && truncated[0]))};
 wire signed [63:0] term_floor=term_product>>>14;
 wire signed [63:0] term_rounded=term_floor+
  $signed({63'd0,(term_product[13:0]>14'd8192 ||
   (term_product[13:0]==14'd8192 && term_floor[0]))});
 wire signed [63:0] ordered_acc=first_pipe[8]?64'sd0:accumulator;
 wire signed [64:0] ordered_sum=$signed({ordered_acc[63],ordered_acc})+
  $signed({rounded_term[63],rounded_term});
 wire sum_sat=ordered_sum[64]!=ordered_sum[63];
 wire row_sat=sum_hold[63:31]!={33{sum_hold[31]}};
 wire [2:0] retire_sats={1'b0,sat_pipe[9]}+{2'd0,(last_pipe[9] && row_sat)};
 wire [31:0] retire_flags=(retire_sats!=0?32'd2:32'd0)|(under_pipe[9]!=0?32'd4:32'd0);
 function automatic [132:0] div3(input [62:0] din,input [62:0] qin,input [6:0] minput);
  reg [62:0] dd,qq;reg [6:0] mm,digit;reg [7:0] total;integer step;
  begin
   dd=din;qq=qin;mm=minput;
   for(step=0;step<3;step=step+1)begin
    total={1'b0,mm}+{1'b0,dd[62:56]};
    digit=mm+(total>=127?7'd1:7'd0);
    mm=total>=127?total[6:0]-7'd127:total[6:0];
    qq=(qq<<7)|{56'd0,digit};dd=dd<<7;
   end
   div3={dd,qq,mm};
  end
 endfunction
 assign in_ready=!rst && !clear && !fault_seen;
 integer i;
 always @(posedge clk or posedge rst)begin
  if(rst)begin
   valid_pipe<=0;first_pipe<=0;last_pipe<=0;bad_pipe<=0;fault_seen<=0;
   term_done<=0;row_done<=0;invalid_scale<=0;row_result<=0;
   numeric_flags<=0;saturation_count<=0;underflow_count<=0;invalid_count<=0;accumulator<=0;
  end else if(clear)begin
   valid_pipe<=0;first_pipe<=0;last_pipe<=0;bad_pipe<=0;fault_seen<=0;
   term_done<=0;row_done<=0;invalid_scale<=0;row_result<=0;
   numeric_flags<=0;saturation_count<=0;underflow_count<=0;invalid_count<=0;accumulator<=0;
  end else begin
   valid_pipe<={valid_pipe[8:0],in_valid && in_ready};
   first_pipe<={first_pipe[7:0],first_group};last_pipe<={last_pipe[8:0],last_group};
   bad_pipe<={bad_pipe[8:0],(scale_bits[15]||scale_bits[14:10]==31||scale_bits[14:0]==0)};
   term_done<=0;row_done<=0;invalid_scale<=0;
   // Pipeline0: one coefficient multiply. Following metadata stays ordered.
   if(in_valid && in_ready)begin
    coefficient_product<={scale_bits[14:10]!=0,scale_bits[9:0]}*maximum;
    exponent<=scale_bits[14:10];partial_pipe[0]<=partial_sum;
   end
   for(i=1;i<7;i=i+1)if(valid_pipe[i-1])partial_pipe[i]<=partial_pipe[i-1];
   // Pipeline1: exact exponent adjustment; max left shift is19.
   if(valid_pipe[0])begin
    if(exponent>=11)begin
     numerator[1]<={20'd0,coefficient_product}<<(exponent-5'd11);shift_right[1]<=0;
    end else begin
     numerator[1]<={20'd0,coefficient_product};
     shift_right[1]<=exponent==0?4'd10:(4'd11-exponent[3:0]);
    end
   end
   // Pipeline2..4: nine base128 digits divide by127, three small digits/stage.
   if(valid_pipe[1])begin
    {digits[2],quotient[2],modulo[2]}<=div3(numerator[1],63'd0,7'd0);
    numerator[2]<=numerator[1];shift_right[2]<=shift_right[1];
   end
   for(i=3;i<5;i=i+1)if(valid_pipe[i-1])begin
    {digits[i],quotient[i],modulo[i]}<=div3(digits[i-1],quotient[i-1],modulo[i-1]);
    numerator[i]<=numerator[i-1];shift_right[i]<=shift_right[i-1];
   end
   // Pipeline5..6: coefficient RNE and int32 saturation, not FP16 conversion.
   if(valid_pipe[4])begin
    truncated<=shifted_quotient;
    remainder<=numerator[4]-((shifted_quotient<<shift_right[4])<<7)+(shifted_quotient<<shift_right[4]);
    denominator<=63'd127<<shift_right[4];nonzero_numerator<=numerator[4]!=0;
   end
   if(valid_pipe[5])begin
    coefficient<=rounded_coefficient>63'h7fffffff ? 32'sh7fffffff : $signed(rounded_coefficient[31:0]);
    coefficient_sat<=rounded_coefficient>63'h7fffffff;
    coefficient_under<=nonzero_numerator && rounded_coefficient==0;
   end
   // Pipeline7: exactly one signed32x32 multiply.
   if(valid_pipe[6])begin
    term_product<=partial_pipe[6]*coefficient;
    sat_pipe[7]<={1'b0,coefficient_sat};under_pipe[7]<={1'b0,coefficient_under};
   end
   // Pipeline8: each group RNE14 BEFORE ordered accumulation.
   if(valid_pipe[7])begin
    rounded_term<=term_rounded;sat_pipe[8]<=sat_pipe[7];
    under_pipe[8]<=under_pipe[7]+{1'b0,(term_product!=0 && term_rounded==0)};
   end
   // Pipeline9: one exact saturating add64, retaining original group order.
   if(valid_pipe[8])begin
    sum_hold<=sum_sat?(ordered_sum[64]?64'sh8000000000000000:64'sh7fffffffffffffff):ordered_sum[63:0];
    if(!bad_pipe[8])accumulator<=sum_sat?(ordered_sum[64]?64'sh8000000000000000:64'sh7fffffffffffffff):ordered_sum[63:0];
    sat_pipe[9]<=sat_pipe[8]+{1'b0,sum_sat};under_pipe[9]<=under_pipe[8];
   end
   // Pipeline10: retire diagnostics in issue order, including invalid input.
   if(valid_pipe[9] && !fault_seen)begin
    if(bad_pipe[9])begin
     numeric_flags<=8;saturation_count<=0;underflow_count<=0;invalid_count<=1;
     invalid_scale<=1;fault_seen<=1;
    end else begin
     numeric_flags<=numeric_flags|retire_flags;
     saturation_count<=saturation_count+{29'd0,retire_sats};
     underflow_count<=underflow_count+{30'd0,under_pipe[9]};
     term_done<=1;row_done<=last_pipe[9];
     if(last_pipe[9])row_result<=row_sat?(sum_hold[63]?32'sh80000000:32'sh7fffffff):sum_hold[31:0];
    end
   end
  end
 end
endmodule
