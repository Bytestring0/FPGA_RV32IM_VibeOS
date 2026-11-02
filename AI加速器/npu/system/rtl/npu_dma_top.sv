// One outstanding 256-bit DDR-line transaction; default16, opt-in32/64 W4A8 lanes.
// Byte offsets are relative to a 1-GiB DDR window. No scaled final result is implied.
module npu_dma_top #(
    parameter logic [31:0] CSR_BASE = 32'h1000_4000,
    parameter bit PIPELINED_CORE = 1,
    parameter bit REUSE_ACT_LINE = 0,
    parameter integer LANES = 16
) (
    input  wire clk, rst,
    input  wire req_valid, output wire req_ready, input wire req_write,
    input  wire [31:0] req_addr, req_wdata, input wire [3:0] req_wstrb,
    output reg rsp_valid, input wire rsp_ready, output reg [31:0] rsp_rdata,
    output reg rsp_error,
    output wire mem_req_valid, input wire mem_req_ready,
    output wire [29:0] mem_req_addr, output wire mem_req_write,
    output wire [255:0] mem_req_wdata, output wire [31:0] mem_req_wstrb,
    input wire mem_rsp_valid, output wire mem_rsp_ready,
    input wire [255:0] mem_rsp_data, input wire mem_rsp_error,
    output reg busy, done, error, output reg [31:0] error_code,
    output reg [31:0] cycle_count, result_count
);
    localparam [3:0] IDLE=0, ACT_REQ=1, ACT_RSP=2, WEIGHT_REQ=3,
                     WEIGHT_RSP=4, ACT_FETCH=5, ISSUE=6, RESULT=7,
                     OUT_REQ=8, OUT_RSP=9, VALIDATE_MUL=10, VALIDATE_SAVE=11, VALIDATE_FINAL=12,
                     ACT_FETCH_HIGH=13;
    localparam integer BEATS_PER_GROUP=128/LANES;
    localparam [2:0] LAST_BEAT=BEATS_PER_GROUP[2:0]-3'd1;
    localparam [31:0] BAD_N=1, BAD_K=2, BAD_ALIGN=3, BAD_STRIDE=4,
                      BAD_RANGE=5, BAD_OVERLAP=6, ACT_FAULT=7,
                      WEIGHT_FAULT=8, WRITE_FAULT=9;
    reg [3:0] state;
    reg [31:0] cfg_n, cfg_k, cfg_weight, cfg_activation, cfg_output,
               cfg_weight_hi, cfg_activation_hi, cfg_output_hi,
               cfg_weight_stride, cfg_activation_stride, cfg_output_stride;
    wire [31:0] cfg_groups = (cfg_k + 32'd127) >> 7;
    wire [31:0] cfg_wstride = cfg_weight_stride == 0 ? (cfg_groups << 6) : cfg_weight_stride;
    wire [31:0] cfg_astride = cfg_activation_stride == 0 ? 32'd32 : cfg_activation_stride;
    wire [31:0] cfg_ostride = cfg_output_stride == 0 ? ((cfg_groups + 32'd3) >> 2) << 5 : cfg_output_stride;
    // Configuration validation is deliberately iterative. No three wide
    // combinational multipliers sit between MMIO registers and START state.
    reg [31:0] validation_weight,validation_activation,validation_output;
    reg [63:0] w_end,a_end,o_end,validation_product,validation_term;
    reg [15:0] validation_multiplier;
    reg [1:0] validation_kind;
    wire [63:0] w_begin={32'd0,validation_weight};
    wire [63:0] a_begin={32'd0,validation_activation};
    wire [63:0] o_begin={32'd0,validation_output};
    wire overlap_wa = w_begin < a_end && a_begin < w_end;
    wire overlap_wo = w_begin < o_end && o_begin < w_end;
    wire overlap_ao = a_begin < o_end && o_begin < a_end;
    reg [31:0] validation;
    always @* begin
        validation = 0;
        if (cfg_n == 0 || cfg_n > 32'd65535) validation = BAD_N;
        else if (cfg_k == 0 || cfg_k > 32'd4096) validation = BAD_K;
        else if (|cfg_weight_hi || |cfg_activation_hi || |cfg_output_hi ||
                 |cfg_weight[4:0] || |cfg_activation[4:0] || |cfg_output[4:0]) validation = BAD_ALIGN;
        else if (cfg_wstride < (cfg_groups<<6) || |cfg_wstride[4:0] ||
                 cfg_astride < 32'd32 || |cfg_astride[4:0] ||
                 cfg_ostride < (cfg_groups<<3) || |cfg_ostride[4:0]) validation = BAD_STRIDE;
    end

    reg [15:0] run_n, row_idx;
    reg [12:0] run_k;
    reg [5:0] run_groups, group_idx;
    reg [2:0] beat_idx;
    reg [7:0] act_lines, fill_idx;
    reg [29:0] weight_stride, activation_stride, output_stride;
    reg [29:0] activation_address, row_weight_address, row_output_address;
    reg [29:0] group_weight_address, group_output_address;
    // 4096 INT8 activation bytes = 128 x 256-bit lines; clocked BRAM read.
    reg [255:0] activation_cache [0:127];
    reg [255:0] act_line, weight_line, pack_data;
    reg [31:0] pack_strobe;
    reg [29:0] pack_addr;
    reg [2:0] pack_count; // 0..4 records not yet committed
    reg last_result;
    wire [6:0] act_index;
    wire [29:0] weight_address;
    wire weight_line_last;
    wire [29:0] output_address = group_output_address;
    wire [31:0] element_base = ({26'd0,group_idx} << 7) + ({29'd0,beat_idx} << $clog2(LANES));
    wire [LANES*4-1:0] selected_weight;
    wire [LANES*8-1:0] selected_act;
    wire [LANES*4-1:0] core_weight;
    wire [LANES*8-1:0] core_activation;
    wire [6:0] cache_read_index=state==ACT_FETCH_HIGH?act_index+7'd1:act_index;
    // Exactly one256-bit synchronous cache read port. The64-lane mode reads
    // low/high lines on successive states and retains the first line locally.
    always @(posedge clk) begin
        if(state==ACT_RSP && mem_rsp_valid && mem_rsp_ready && !mem_rsp_error)
            activation_cache[fill_idx[6:0]]<=mem_rsp_data;
        if(state==ACT_FETCH || (LANES==64 && state==ACT_FETCH_HIGH))
            act_line<=activation_cache[cache_read_index];
    end
    generate if(LANES==16) begin : supply16
        assign act_index={group_idx[4:0],2'b0}+{5'd0,beat_idx[2:1]};
        assign weight_address=group_weight_address+(beat_idx[2]?30'd32:30'd0);
        assign weight_line_last=beat_idx[1:0]==2'd3;
        assign selected_weight=weight_line[(beat_idx[1:0]*64)+:64];
        assign selected_act=beat_idx[0]?act_line[255:128]:act_line[127:0];
    end else if(LANES==32) begin : supply32
        assign act_index={group_idx[4:0],2'b0}+{4'd0,beat_idx};
        assign weight_address=group_weight_address+(beat_idx[1]?30'd32:30'd0);
        assign weight_line_last=beat_idx[0];
        assign selected_weight=weight_line[(beat_idx[0]*128)+:128];
        assign selected_act=act_line;
    end else begin : supply64
        reg [255:0] act_low_line;
        // A real second single-port BRAM read supplies the high32 activations.
        // No fictitious512-bit cache port or additional external DDR port.
        always @(posedge clk)
            if(state==ACT_FETCH_HIGH) act_low_line<=act_line;
        assign act_index={group_idx[4:0],2'b0}+{3'd0,beat_idx,1'b0};
        assign weight_address=group_weight_address+({27'd0,beat_idx}<<5);
        assign weight_line_last=1'b1;
        assign selected_weight=weight_line;
        assign selected_act={act_line,act_low_line};
    end endgenerate
    genvar lane;
    generate for (lane=0; lane<LANES; lane=lane+1) begin : mask_tail
        wire valid_lane = element_base + lane < {19'd0,run_k};
        assign core_weight[4*lane +: 4] = valid_lane ? selected_weight[4*lane +: 4] : 4'd0;
        assign core_activation[8*lane +: 8] = valid_lane ? selected_act[8*lane +: 8] : 8'd0;
    end endgenerate
    wire core_ready, core_out_valid;
    wire signed [31:0] core_sum;
    wire [15:0] core_row, core_group;
    generate if (PIPELINED_CORE && LANES==16) begin : pipelined_compute
        gemv_pipeline16 core (
            .clk(clk), .rst_n(~rst), .in_valid(state==ISSUE && busy), .in_ready(core_ready),
            .weight_data(core_weight), .activation_data(core_activation),
            .group_first(beat_idx==0), .group_last(beat_idx==LAST_BEAT),
            .row_id(row_idx), .group_id({10'd0,group_idx}),
            .out_valid(core_out_valid), .out_ready(state==RESULT && busy),
            .partial_sum(core_sum), .out_row_id(core_row), .out_group_id(core_group)
        );
    end else if(PIPELINED_CORE) begin : wide_pipelined_compute
        gemv_pipeline_v2 #(.LANES(LANES)) core (
            .clk(clk), .rst_n(~rst), .in_valid(state==ISSUE && busy), .in_ready(core_ready),
            .weight_data(core_weight), .activation_data(core_activation),
            .group_first(beat_idx==0), .group_last(beat_idx==LAST_BEAT),
            .row_id(row_idx), .group_id({10'd0,group_idx}),
            .out_valid(core_out_valid), .out_ready(state==RESULT && busy),
            .partial_sum(core_sum), .out_row_id(core_row), .out_group_id(core_group)
        );
    end else begin : original_compute
        gemv_core #(.LANES(LANES)) core (
            .clk(clk), .rst_n(~rst), .in_valid(state==ISSUE && busy), .in_ready(core_ready),
            .weight_data(core_weight), .activation_data(core_activation),
            .group_first(beat_idx==0), .group_last(beat_idx==LAST_BEAT),
            .row_id(row_idx), .group_id({10'd0,group_idx}),
            .out_valid(core_out_valid), .out_ready(state==RESULT && busy),
            .partial_sum(core_sum), .out_row_id(core_row), .out_group_id(core_group)
        );
    end endgenerate
    assign mem_req_valid = !rst && busy &&
        (state==ACT_REQ || state==WEIGHT_REQ || state==OUT_REQ);
    assign mem_req_write = state==OUT_REQ;
    assign mem_req_addr = state==ACT_REQ ? activation_address[29:0] :
                          state==WEIGHT_REQ ? weight_address[29:0] : pack_addr;
    assign mem_req_wdata = pack_data;
    assign mem_req_wstrb = state==OUT_REQ ? pack_strobe : 32'd0;
    assign mem_rsp_ready = !rst && busy &&
        (state==ACT_RSP || state==WEIGHT_RSP || state==OUT_RSP);
    assign req_ready = !rst && !rsp_valid;

    always @(posedge clk or posedge rst) begin
        if (rst) begin
            rsp_valid<=0; rsp_rdata<=0; rsp_error<=0;
            cfg_n<=0; cfg_k<=0; cfg_weight<=0; cfg_activation<=0; cfg_output<=0;
            cfg_weight_hi<=0; cfg_activation_hi<=0; cfg_output_hi<=0;
            cfg_weight_stride<=0; cfg_activation_stride<=0; cfg_output_stride<=0;
            state<=IDLE; busy<=0; done<=0; error<=0; error_code<=0;
            cycle_count<=0; result_count<=0;
            run_n<=0; run_k<=0; run_groups<=0; row_idx<=0; group_idx<=0;
            beat_idx<=0; act_lines<=0; fill_idx<=0;
            activation_address<=0; row_weight_address<=0; row_output_address<=0;
            group_weight_address<=0; group_output_address<=0;
            weight_stride<=0; activation_stride<=0; output_stride<=0;
            weight_line<=0; pack_data<=0; pack_strobe<=0;
            pack_addr<=0; pack_count<=0; last_result<=0;
            validation_weight<=0;validation_activation<=0;validation_output<=0;
            w_end<=0;a_end<=0;o_end<=0;validation_product<=0;validation_term<=0;
            validation_multiplier<=0;validation_kind<=0;
        end else begin
            if (rsp_valid && rsp_ready) rsp_valid<=0;
            if (busy) cycle_count<=cycle_count+32'd1;
            if (req_valid && req_ready) begin
                rsp_valid<=1; rsp_rdata<=0; rsp_error<=0;
                if (req_addr[31:12]!=CSR_BASE[31:12] || |req_addr[1:0]) begin
                    rsp_error<=1;
                end else if (req_write) begin
                    if (req_wstrb!=4'hf) rsp_error<=1;
                    else case (req_addr[11:0])
                        12'h000: begin
                            if (req_wdata==32'd2 && !busy) begin
                                done<=0; error<=0; error_code<=0; cycle_count<=0; result_count<=0;
                            end else if (req_wdata==32'd1 && !busy && !done && !error) begin
                                if (validation!=0) begin
                                    error<=1; error_code<=validation;
                                end else begin
                                    run_n<=cfg_n[15:0]; run_k<=cfg_k[12:0];
                                    run_groups<=cfg_groups[5:0];
                                    row_weight_address<=cfg_weight[29:0]; group_weight_address<=cfg_weight[29:0];
                                    row_output_address<=cfg_output[29:0]; group_output_address<=cfg_output[29:0];
                                    activation_address<=cfg_activation[29:0]; weight_stride<=cfg_wstride[29:0];
                                    activation_stride<=cfg_astride[29:0]; output_stride<=cfg_ostride[29:0];
                                    act_lines<=cfg_k[12:5] + {7'd0,(|cfg_k[4:0])}; fill_idx<=0;
                                    row_idx<=0; group_idx<=0; beat_idx<=0;
                                    pack_count<=0; pack_data<=0; pack_strobe<=0;
                                    last_result<=0; busy<=1; state<=VALIDATE_MUL;
                                     validation_weight<=cfg_weight;validation_activation<=cfg_activation;
                                     validation_output<=cfg_output;validation_kind<=0;
                                     validation_product<=0;validation_term<={32'd0,cfg_wstride};
                                     validation_multiplier<=cfg_n[15:0]-16'd1;
                                    cycle_count<=0; result_count<=0;
                                end
                            end else rsp_error<=1;
                        end
                        12'h008: if (!busy) cfg_n<=req_wdata; else rsp_error<=1;
                        12'h00c: if (!busy) cfg_k<=req_wdata; else rsp_error<=1;
                        12'h010: if (!busy) cfg_weight<=req_wdata; else rsp_error<=1;
                        12'h014: if (!busy) cfg_weight_hi<=req_wdata; else rsp_error<=1;
                        12'h018: if (!busy) cfg_activation<=req_wdata; else rsp_error<=1;
                        12'h01c: if (!busy) cfg_activation_hi<=req_wdata; else rsp_error<=1;
                        12'h020: if (!busy) cfg_output<=req_wdata; else rsp_error<=1;
                        12'h024: if (!busy) cfg_output_hi<=req_wdata; else rsp_error<=1;
                        12'h028: if (!busy) cfg_weight_stride<=req_wdata; else rsp_error<=1;
                        12'h02c: if (!busy) cfg_activation_stride<=req_wdata; else rsp_error<=1;
                        12'h030: if (!busy) cfg_output_stride<=req_wdata; else rsp_error<=1;
                        default: rsp_error<=1;
                    endcase
                end else case (req_addr[11:0])
                    12'h000: rsp_rdata<=0;
                    12'h004: rsp_rdata<={29'd0,error,done,busy};
                    12'h008: rsp_rdata<=cfg_n;
                    12'h00c: rsp_rdata<=cfg_k;
                    12'h010: rsp_rdata<=cfg_weight;
                    12'h014: rsp_rdata<=cfg_weight_hi;
                    12'h018: rsp_rdata<=cfg_activation;
                    12'h01c: rsp_rdata<=cfg_activation_hi;
                    12'h020: rsp_rdata<=cfg_output;
                    12'h024: rsp_rdata<=cfg_output_hi;
                    12'h028: rsp_rdata<=cfg_weight_stride;
                    12'h02c: rsp_rdata<=cfg_activation_stride;
                    12'h030: rsp_rdata<=cfg_output_stride;
                    12'h034: rsp_rdata<=error_code;
                    12'h038: rsp_rdata<=cycle_count;
                    12'h03c: rsp_rdata<=result_count;
                    12'h040: rsp_rdata<=32'h4e444d41;
                    12'h044: rsp_rdata<=cfg_groups;
                    12'h048: rsp_rdata<=LANES;
                    default: rsp_error<=1;
                endcase
            end
            case (state)
                VALIDATE_MUL: begin
                    if (validation_multiplier==0) state<=VALIDATE_SAVE;
                    else begin
                        if (validation_multiplier[0]) validation_product<=validation_product+validation_term;
                        validation_term<=validation_term<<1;
                        validation_multiplier<=validation_multiplier>>1;
                    end
                end
                VALIDATE_SAVE: begin
                    validation_product<=0;
                    if (validation_kind==0) begin
                        w_end<=w_begin+validation_product+({58'd0,run_groups}<<6);
                        validation_kind<=1;validation_term<={32'd0,cfg_astride};
                        validation_multiplier<={8'd0,act_lines};state<=VALIDATE_MUL;
                    end else if (validation_kind==1) begin
                        a_end<=a_begin+validation_product;
                        validation_kind<=2;validation_term<={32'd0,cfg_ostride};
                        validation_multiplier<=run_n-16'd1;state<=VALIDATE_MUL;
                    end else begin
                        o_end<=o_begin+validation_product+({58'd0,run_groups}<<3);
                        state<=VALIDATE_FINAL;
                    end
                end
                VALIDATE_FINAL: begin
                    if (w_end>64'h4000_0000 || a_end>64'h4000_0000 || o_end>64'h4000_0000) begin
                        state<=IDLE;busy<=0;error<=1;error_code<=BAD_RANGE;
                    end else if (overlap_wa || overlap_wo || overlap_ao) begin
                        state<=IDLE;busy<=0;error<=1;error_code<=BAD_OVERLAP;
                    end else state<=ACT_REQ;
                end
                ACT_REQ: if (mem_req_valid && mem_req_ready) state<=ACT_RSP;
                ACT_RSP: if (mem_rsp_valid && mem_rsp_ready) begin
                    if (mem_rsp_error) begin
                        state<=IDLE; busy<=0; error<=1; error_code<=ACT_FAULT;
                    end else begin
                        if (fill_idx+8'd1==act_lines) begin
                            beat_idx<=0; activation_address<=activation_address; state<=WEIGHT_REQ;
                        end else begin fill_idx<=fill_idx+8'd1; activation_address<=activation_address+activation_stride; state<=ACT_REQ; end
                    end
                end
                WEIGHT_REQ: if (mem_req_valid && mem_req_ready) state<=WEIGHT_RSP;
                WEIGHT_RSP: if (mem_rsp_valid && mem_rsp_ready) begin
                    if (mem_rsp_error) begin
                        state<=IDLE; busy<=0; error<=1; error_code<=WEIGHT_FAULT;
                    end else begin weight_line<=mem_rsp_data; state<=ACT_FETCH; end
                end
                ACT_FETCH: begin
                    state<=LANES==64?ACT_FETCH_HIGH:ISSUE;
                end
                ACT_FETCH_HIGH: state<=ISSUE;
                ISSUE: if (core_ready) begin
                    if (beat_idx==LAST_BEAT) state<=RESULT;
                    else if (weight_line_last) begin
                        beat_idx<=beat_idx+3'd1; state<=WEIGHT_REQ;
                    end else begin
                        beat_idx<=beat_idx+3'd1;
                        // Even/odd beats select the two halves of the same cached line.
                        // Advance only on core_ready; odd beats still fetch the next line.
                        state<=(LANES==16 && REUSE_ACT_LINE && !beat_idx[0]) ? ISSUE : ACT_FETCH;
                    end
                end
                RESULT: if (core_out_valid) begin
                    // The core's own tags, not reconstructed tags, go to DDR.
                    pack_data[pack_count*64 +: 64]<={core_sum,core_group,core_row};
                    pack_strobe[pack_count*8 +: 8]<=8'hff;
                    if (pack_count==0) pack_addr<=output_address[29:0];
                    pack_count<=pack_count+3'd1;
                    last_result <= row_idx==run_n-16'd1 && group_idx==run_groups-6'd1;
                    if (group_idx==run_groups-6'd1) begin
                        group_idx<=0; row_idx<=row_idx+16'd1;
                        row_weight_address<=row_weight_address+weight_stride;
                        group_weight_address<=row_weight_address+weight_stride;
                        row_output_address<=row_output_address+output_stride;
                        group_output_address<=row_output_address+output_stride;
                    end else begin
                        group_idx<=group_idx+6'd1;
                        group_weight_address<=group_weight_address+30'd64;
                        group_output_address<=group_output_address+30'd8;
                    end
                    beat_idx<=0;
                    if (pack_count==3 || group_idx==run_groups-6'd1) state<=OUT_REQ;
                    else state<=WEIGHT_REQ;
                end
                OUT_REQ: if (mem_req_valid && mem_req_ready) state<=OUT_RSP;
                OUT_RSP: if (mem_rsp_valid && mem_rsp_ready) begin
                    if (mem_rsp_error) begin
                        state<=IDLE; busy<=0; error<=1; error_code<=WRITE_FAULT;
                    end else begin
                        result_count<=result_count+{29'd0,pack_count};
                        pack_count<=0; pack_data<=0; pack_strobe<=0;
                        if (last_result) begin state<=IDLE; busy<=0; done<=1; end
                        else state<=WEIGHT_REQ;
                    end
                end
                IDLE: begin end
                default: state<=IDLE;
            endcase
        end
    end
endmodule
