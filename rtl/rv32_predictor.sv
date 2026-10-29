// Tagged BTB, per-entry local history, banked two-bit direction counters, RAS.
// Independent single-writer banks preserve LUT-RAM inference in PDS ADS.
module rv32_predictor #(
    parameter integer INDEX_BITS = 6,
    parameter bit RAS_ENABLE = 1,
    parameter bit LOCAL_HISTORY = 1,
    parameter integer HISTORY_BITS = 3
) (
    input logic clk, rst, clear,
    input logic [31:0] pc, pc_second,
    output logic [31:0] next_pc, next_pc_second,
    output logic next_pc_sequential,
    input logic update,
    input logic [31:0] update_pc, update_target,
    input logic update_conditional, update_taken,
    input logic update_call, update_return
);
    localparam integer N = 1 << INDEX_BITS;
    logic [N-1:0] valid;
    logic [29-INDEX_BITS:0] tags [0:N-1];
    logic [31:0] targets [0:N-1];
    logic [1:0] counters [0:N-1];
    logic [HISTORY_BITS-1:0] history [0:N-1];
    wire [(1<<HISTORY_BITS)-1:0] local_direction, local_direction_second;
    wire direction = LOCAL_HISTORY ? local_direction[history[ri]] : counters[ri][1];
    wire direction_second = LOCAL_HISTORY ? local_direction_second[history[ri_second]] : counters[ri_second][1];
    logic conditional [0:N-1];
    logic is_return [0:N-1];
    logic [31:0] return_stack [0:15];
    logic [3:0] return_pointer;
    logic [4:0] return_count;
    wire [3:0] return_top = return_pointer - 4'd1;
    wire [INDEX_BITS-1:0] ri = pc[INDEX_BITS+1:2];
    wire [INDEX_BITS-1:0] ri_second = pc_second[INDEX_BITS+1:2];
    wire [INDEX_BITS-1:0] wi = update_pc[INDEX_BITS+1:2];
    generate for (genvar bank = 0; bank < (1<<HISTORY_BITS); bank = bank+1) begin : direction_bank
        localparam logic [HISTORY_BITS-1:0] BANK_ID = bank;
        logic [1:0] values [0:N-1];
        assign local_direction[bank] = values[ri][1];
        assign local_direction_second[bank] = values[ri_second][1];
        always_ff @(posedge clk) begin
            if (LOCAL_HISTORY && !rst && !clear && update) begin
                if (!valid[wi] || tags[wi] != update_pc[31:INDEX_BITS+2])
                    values[wi] <= update_taken ? 2'b10 : 2'b01;
                else if (update_conditional && history[wi] == BANK_ID) begin
                    if (update_taken && values[wi] != 3) values[wi] <= values[wi] + 2'd1;
                    else if (!update_taken && values[wi] != 0) values[wi] <= values[wi] - 2'd1;
                end
            end
        end
    end endgenerate
    wire predict_override=valid[ri] && tags[ri]==pc[31:INDEX_BITS+2] && (!conditional[ri] || direction);
    wire [31:0] predicted_target=(RAS_ENABLE && is_return[ri] && return_count!=0)?return_stack[return_top]:targets[ri];
    always_comb begin
        next_pc=predict_override?predicted_target:pc+32'd4;
        // Test the target before the 32-bit default/target selection.
        next_pc_sequential=!predict_override || predicted_target==pc+32'd4;
        next_pc_second = pc_second + 32'd4;
        if (valid[ri_second] && tags[ri_second] == pc_second[31:INDEX_BITS+2] &&
            (!conditional[ri_second] || direction_second))
            next_pc_second = (RAS_ENABLE && is_return[ri_second] && return_count != 0) ?
                             return_stack[return_top] : targets[ri_second];
    end
    always_ff @(posedge clk) begin
        if (rst || clear) begin
            valid <= '0;
            return_pointer <= 0; return_count <= 0;
        end else if (update) begin
            valid[wi] <= 1;
            tags[wi] <= update_pc[31:INDEX_BITS+2];
            targets[wi] <= update_target;
            conditional[wi] <= update_conditional;
            is_return[wi] <= update_return;
            if (LOCAL_HISTORY) begin
                if (!valid[wi] || tags[wi] != update_pc[31:INDEX_BITS+2]) begin
                    history[wi] <= {{(HISTORY_BITS-1){1'b0}},update_taken};
                end else if (update_conditional) begin
                    history[wi] <= {history[wi][HISTORY_BITS-2:0],update_taken};
                end
            end
            // Execution-updated RAS: no speculative stack rollback is needed.
            // A very short call/return chain may still miss and is corrected
            // by the normal architectural next-PC check in EX.
            if (RAS_ENABLE && update_call) begin
                return_stack[return_pointer] <= update_pc + 32'd4;
                return_pointer <= return_pointer + 4'd1;
                if (return_count != 16) return_count <= return_count + 5'd1;
            end else if (RAS_ENABLE && update_return && return_count != 0) begin
                return_pointer <= return_pointer - 4'd1;
                return_count <= return_count - 5'd1;
            end
            if (!valid[wi] || tags[wi] != update_pc[31:INDEX_BITS+2])
                counters[wi] <= update_taken ? 2'b10 : 2'b01;
            else if (update_taken && counters[wi] != 3)
                counters[wi] <= counters[wi] + 2'd1;
            else if (!update_taken && counters[wi] != 0)
                counters[wi] <= counters[wi] - 2'd1;
        end
    end
endmodule
