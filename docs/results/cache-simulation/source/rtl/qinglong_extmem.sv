// CPU-side adapter for the 1 GiB DDR window. One 32-byte transaction at a
// time. Optional instruction stream buffer; backing I/D caches sit downstream.
// Instruction fetches crossing a line use two transactions.
module qinglong_extmem #(parameter bit I_BUFFER=0) (
    input logic clk, rst, input logic invalidate_i,
    output logic [31:0] i_buffer_hits,
    input logic i_req_valid, output logic i_req_ready,
    input logic [31:0] i_req_addr,
    output logic i_rsp_valid, input logic i_rsp_ready,
    output logic [63:0] i_rsp_data, output logic [1:0] i_rsp_error,
    input logic d_req_valid, output logic d_req_ready,
    input logic [31:0] d_req_addr, d_req_wdata,
    input logic [3:0] d_req_wstrb, input logic d_req_write,
    output logic d_rsp_valid, input logic d_rsp_ready,
    output logic [31:0] d_rsp_data, output logic d_rsp_error,
    output logic req_valid, input logic req_ready,
    output logic [29:0] req_addr,
    output logic req_write, output logic req_instruction, output logic [255:0] req_wdata,
    output logic [31:0] req_wstrb,
    input logic rsp_valid, output logic rsp_ready,
    input logic [255:0] rsp_data, input logic rsp_error
);
    localparam logic [1:0] IDLE=0, SEND=1, WAIT_RSP=2;
    logic [1:0] state;
    logic prefer_i, owner_i, second_line, last_word;
    logic [2:0] lane;
    logic choose_i;
    logic ib_valid;
    logic [29:0] ib_addr;
    logic [255:0] ib_data;
    wire ib_hit=I_BUFFER && ib_valid && !invalidate_i && i_req_addr[31:30]==2'b10 &&
        i_req_addr[1:0]==0 && i_req_addr[4:2]!=7 && i_req_addr[29:5]==ib_addr[29:5];
    wire [255:0] shifted = rsp_data >> {lane, 5'b0};
    // Registered round-robin grant: neither ready depends on the other
    // request's address/valid. An idle unrequested turn costs at most one cycle.
    wire eligible_i=i_req_valid && (!i_rsp_valid || (I_BUFFER && i_rsp_ready));
    wire eligible_d=d_req_valid && !d_rsp_valid;
    assign choose_i = eligible_i && (!eligible_d || prefer_i);
    assign i_req_ready = !rst && state == IDLE && (!i_rsp_valid || (I_BUFFER && i_rsp_ready)) && choose_i;
    assign d_req_ready = !rst && state == IDLE && !d_rsp_valid && !choose_i;
    assign req_instruction = owner_i;
    assign req_valid = !rst && state == SEND;
    assign rsp_ready = !rst && state == WAIT_RSP;
    always_ff @(posedge clk) begin
        if (rst) begin
            ib_valid<=0;ib_addr<=0;ib_data<=0;i_buffer_hits<=0;
            state <= IDLE; prefer_i <= 0; owner_i <= 0; second_line <= 0;
            last_word <= 0; lane <= 0; req_addr <= 0; req_write <= 0;
            req_wdata <= 0; req_wstrb <= 0;
            i_rsp_valid <= 0; i_rsp_data <= 0; i_rsp_error <= 0;
            d_rsp_valid <= 0; d_rsp_data <= 0; d_rsp_error <= 0;
        end else begin
            if(invalidate_i) ib_valid<=0;
            if (i_rsp_ready) i_rsp_valid <= 0;
            if (d_rsp_ready) d_rsp_valid <= 0;
            if (state == IDLE && !i_rsp_valid && !d_rsp_valid)
                prefer_i <= !prefer_i;
            case (state)
                IDLE: if ((i_req_valid && i_req_ready) || (d_req_valid && d_req_ready)) begin
                    owner_i <= choose_i; prefer_i <= !choose_i; second_line <= 0;
                    req_write <= !choose_i && d_req_write;
                    req_wdata <= {8{d_req_wdata}};
                    req_wstrb <= choose_i ? 32'b0 : ({28'b0, d_req_wstrb} << {d_req_addr[4:2], 2'b0});
                    req_addr <= choose_i ? {i_req_addr[29:5],5'b0} : {d_req_addr[29:5],5'b0};
                    lane <= choose_i ? i_req_addr[4:2] : d_req_addr[4:2];
                    last_word <= i_req_addr[29:2] == 28'hfffffff;
                    if(choose_i) i_rsp_error <= 0; else d_rsp_error <= 0;
                    if(choose_i && ib_hit) begin
                        i_rsp_valid<=1;i_rsp_error<=0;
                        i_rsp_data<=ib_data >> {i_req_addr[4:2],5'b0};
                        i_buffer_hits<=i_buffer_hits+1'b1;
                    end else if (choose_i && (i_req_addr[31:30] != 2'b10 || i_req_addr[1:0] != 0)) begin
                        i_rsp_valid <= 1; i_rsp_error <= 3; i_rsp_data <= 0;
                    end else if (!choose_i && d_req_addr[31:30] != 2'b10) begin
                        d_rsp_valid <= 1; d_rsp_error <= 1; d_rsp_data <= 0;
                    end else state <= SEND;
                end
                SEND: if (req_ready) state <= WAIT_RSP;
                WAIT_RSP: if (rsp_valid) begin
                    state <= IDLE;
                    if(owner_i && I_BUFFER && !invalidate_i && !rsp_error) begin
                        ib_valid<=1;ib_addr<=req_addr;ib_data<=rsp_data;
                    end
                    if (!owner_i) begin
                        d_rsp_valid <= 1; d_rsp_data <= shifted[31:0]; d_rsp_error <= rsp_error;
                    end else if (second_line) begin
                        i_rsp_valid <= 1; i_rsp_data[63:32] <= rsp_data[31:0];
                        i_rsp_error[1] <= rsp_error;
                    end else begin
                        i_rsp_data <= shifted[63:0];
                        i_rsp_error <= rsp_error ? 2'b11 : {last_word, 1'b0};
                        if (lane == 7 && !last_word && !rsp_error) begin
                            second_line <= 1; req_addr <= req_addr + 30'd32; state <= SEND;
                        end else i_rsp_valid <= 1;
                    end
                end
                default: state <= IDLE;
            endcase
        end
    end
endmodule
