// A simple conservative four-entry queue, not the optimized project front-end.
module fetch_queue (
    input logic clk, rst, flush,
    input logic push_valid, output logic push_ready,
    input logic [1:0] push_count,
    input logic [31:0] push_pc, push_insn0, push_insn1,
    input logic [1:0] push_error,
    input logic [1:0] pop_count,
    output logic [2:0] count,
    output logic [31:0] pc0, insn0, pc1, insn1,
    output logic error0, error1
);
    logic [31:0] pcs[0:3], insns[0:3];
    logic errors[0:3];
    integer i;
    assign push_ready = !rst && !flush && (push_count == 1 || push_count == 2) &&
                        (count + {1'b0,push_count} <= 4);
    assign pc0=pcs[0]; assign pc1=pcs[1];
    assign insn0=insns[0]; assign insn1=insns[1];
    assign error0=errors[0]; assign error1=errors[1];
    always_ff @(posedge clk) begin
        if (rst || flush) count <= 0;
        else begin
            // Environment must only pop 0, 1, or 2 available entries.
            for (i=0; i<4; i=i+1) begin
                if (i + pop_count < count) begin
                    pcs[i] <= pcs[i+pop_count];
                    insns[i] <= insns[i+pop_count];
                    errors[i] <= errors[i+pop_count];
                end
                if (push_valid && push_ready && i == count-pop_count) begin
                    pcs[i] <= push_pc; insns[i] <= push_insn0; errors[i] <= push_error[0];
                end
                if (push_valid && push_ready && push_count==2 && i == count-pop_count+1) begin
                    pcs[i] <= push_pc+4; insns[i] <= push_insn1; errors[i] <= push_error[1];
                end
            end
            count <= count - pop_count + ((push_valid && push_ready) ? push_count : 0);
        end
    end
endmodule
