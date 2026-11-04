// Four ordered 256-bit weight reads overlap compute; default16, opt-in32/64 lanes.
// Byte offsets relative to 1-GiB DDR. MODE=0 raw-v1 default, MODE=1 fused opcode19 Q16.
module npu_dma_top #(
    parameter logic [31:0] CSR_BASE = 32'h1000_4000,
    parameter bit PIPELINED_CORE = 1,
    parameter bit REUSE_ACT_LINE = 0,
    parameter integer LANES = 16,
    parameter bit WIDE_ACT_CACHE = 1,
    parameter bit STREAM_GROUPS = 1,
    parameter bit FUSED_TILE_STREAM = 1,
    parameter integer WEIGHT_DEPTH = 4
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
    localparam [4:0] IDLE=0, ACT_REQ=1, ACT_RSP=2, WEIGHT_REQ=3,
                     DRAIN=4, ACT_FETCH=5, ISSUE=6, RESULT=7,
                     OUT_REQ=8, OUT_RSP=9, VALIDATE_MUL=10, VALIDATE_SAVE=11, VALIDATE_FINAL=12,
                     ACT_FETCH_HIGH=13, SCALE_REQ=14, SCALE_RSP=15, TILE_SETUP=16, TILE_PLAN=17, TILE_RUN=18, TILE_DRAIN=19, TILE_SCALE_SETUP=20, TILE_SCALE_LOAD=21, TILE_SCALE_DRAIN=22;
    localparam integer BEATS_PER_GROUP=128/LANES;
    localparam [2:0] LAST_BEAT=BEATS_PER_GROUP[2:0]-3'd1;
    localparam [31:0] BAD_N=1, BAD_K=2, BAD_ALIGN=3, BAD_STRIDE=4,
                      BAD_RANGE=5, BAD_OVERLAP=6, ACT_FAULT=7,
                      WEIGHT_FAULT=8, WRITE_FAULT=9, BAD_MODE=10, SCALE_FAULT=11, BAD_SCALE=12;
    reg [4:0] state;
    reg [31:0] cfg_mode, cfg_scale, cfg_maximum;
    wire fused = cfg_mode==1;
    reg [31:0] cfg_n, cfg_k, cfg_weight, cfg_activation, cfg_output,
               cfg_weight_hi, cfg_activation_hi, cfg_output_hi,
               cfg_weight_stride, cfg_activation_stride, cfg_output_stride;
    wire [31:0] cfg_groups = (cfg_k + 32'd127) >> 7;
    wire [31:0] cfg_wstride = cfg_weight_stride == 0 ? (cfg_groups << 6) : cfg_weight_stride;
    wire [31:0] cfg_astride = cfg_activation_stride == 0 ? 32'd32 : cfg_activation_stride;
    wire [31:0] cfg_ostride = fused ? 32'd4 : (cfg_output_stride == 0 ? ((cfg_groups + 32'd3) >> 2) << 5 : cfg_output_stride);
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
                 (!fused && (cfg_ostride < (cfg_groups<<3) || |cfg_ostride[4:0])) ||
                 (fused && cfg_output_stride!=0 && cfg_output_stride!=4)) validation = BAD_STRIDE;
        else if (cfg_mode>1) validation = BAD_MODE;
        else if (fused && (cfg_scale[0] || cfg_maximum>32'h80000000)) validation = BAD_SCALE;
    end

    reg [15:0] run_n, row_idx;
    reg [12:0] run_k;
    reg [5:0] run_groups, group_idx;
    reg [2:0] beat_idx;
    reg [7:0] act_lines, fill_idx;
    reg [29:0] weight_stride, activation_stride, output_stride;
    reg [29:0] activation_address, row_output_address;
    reg [29:0] group_output_address;
    // 4096 INT8 activation bytes = 128 x 256-bit lines; clocked BRAM read.
    localparam bit WIDE_ACT = LANES==64 && WIDE_ACT_CACHE;
    localparam bit TILE_CAPABLE = FUSED_TILE_STREAM && PIPELINED_CORE && WIDE_ACT;
    wire tile_stream=fused&&TILE_CAPABLE;
    wire tile_running=tile_stream&&(state==TILE_RUN||state==TILE_DRAIN);
    reg tile_input_valid;
    reg [15:0] tile_input_row,tile_end_row;
    reg [5:0] tile_input_group;
    reg [2:0] tile_input_beat,tile_plan_row;
    reg [3:0] tile_rows;
    reg [8:0] tile_scale_items;
    reg [9:0] tile_scale_bytes;
    reg [7:0] tile_row_offsets[0:7];
    reg [4:0] tile_last_scale_line;
    wire [15:0] issue_row=tile_stream?tile_input_row:row_idx;
    wire [5:0] issue_group=tile_stream?tile_input_group:group_idx;
    wire [2:0] issue_beat=tile_stream?tile_input_beat:beat_idx;
    wire issue_valid=busy&&(tile_stream?(tile_running&&tile_input_valid):state==ISSUE);
    wire tile_input_ready=!tile_input_valid||core_ready;
    wire [255:0] unused_wide_act_low;
    reg [255:0] act_line, weight_line, pack_data;
    reg [31:0] pack_strobe;
    reg [29:0] pack_addr;
    reg [2:0] issued_batch;
    wire streaming = !fused && STREAM_GROUPS && PIPELINED_CORE && WIDE_ACT;
    reg [3:0] pack_count; // raw: 0..4 records; fused: 0..8 Q16 rows
    reg last_result;
    localparam integer ACT_INDEX_BITS = LANES==64 && WIDE_ACT_CACHE ? 6 : 7;
    wire [ACT_INDEX_BITS-1:0] act_index;
    wire weight_line_last;
    wire [29:0] output_address = group_output_address;
    wire [31:0] element_base = ({26'd0,issue_group} << 7) + ({29'd0,issue_beat} << $clog2(LANES));
    wire [LANES*4-1:0] selected_weight;
    wire [LANES*8-1:0] selected_act;
    wire [LANES*4-1:0] core_weight;
    wire [LANES*8-1:0] core_activation;
    // Exactly one256-bit synchronous cache read port. The64-lane mode reads
    // low/high lines on successive states and retains the first line locally.
    generate if(WIDE_ACT)begin: wide_activation
        // Stripe even/odd lines across two synchronous banks. Read both halves
        // while consuming the weight FIFO; computation sees one registered512-bit vector.
        reg [255:0] low_bank[0:63],high_bank[0:63];
        reg [255:0] wide_act_low;
        assign unused_wide_act_low=wide_act_low;
        always @(posedge clk)begin
            if(state==ACT_RSP && mem_rsp_valid && mem_rsp_ready && !mem_rsp_error)begin
                if(fill_idx[0])high_bank[fill_idx[6:1]]<=mem_rsp_data;
                else low_bank[fill_idx[6:1]]<=mem_rsp_data;
            end
            if(state==WEIGHT_REQ||(tile_stream&&weight_pop))begin
                wide_act_low<=low_bank[act_index];
                act_line<=high_bank[act_index];
            end
        end
    end else begin: narrow_activation
        assign unused_wide_act_low=256'd0;
        wire [6:0] cache_read_index=state==ACT_FETCH_HIGH?act_index+7'd1:act_index;
        reg [255:0] activation_cache[0:127];
        always @(posedge clk)begin
            if(state==ACT_RSP && mem_rsp_valid && mem_rsp_ready && !mem_rsp_error)
                activation_cache[fill_idx[6:0]]<=mem_rsp_data;
            if(state==ACT_FETCH || (LANES==64 && state==ACT_FETCH_HIGH))
                act_line<=activation_cache[cache_read_index];
        end
    end endgenerate
    generate if(LANES==16) begin : supply16
        assign act_index={group_idx[4:0],2'b0}+{5'd0,beat_idx[2:1]};
        assign weight_line_last=beat_idx[1:0]==2'd3;
        assign selected_weight=weight_line[(beat_idx[1:0]*64)+:64];
        assign selected_act=beat_idx[0]?act_line[255:128]:act_line[127:0];
    end else if(LANES==32) begin : supply32
        assign act_index={group_idx[4:0],2'b0}+{4'd0,beat_idx};
        assign weight_line_last=beat_idx[0];
        assign selected_weight=weight_line[(beat_idx[0]*128)+:128];
        assign selected_act=act_line;
    end else begin : supply64
        if(WIDE_ACT)begin: wide_index
            assign act_index={group_idx[4:0],beat_idx[0]};
        end else begin: narrow_index
            assign act_index={group_idx[4:0],2'b0}+{3'd0,beat_idx,1'b0};
        end
        assign weight_line_last=1'b1;
        assign selected_weight=weight_line;
        if(WIDE_ACT)begin: select_wide
            assign selected_act={act_line,unused_wide_act_low};
        end else begin: select_narrow
            reg [255:0] act_low_line;
            always @(posedge clk)if(state==ACT_FETCH_HIGH)act_low_line<=act_line;
            assign selected_act={act_line,act_low_line};
        end
    end endgenerate
    genvar lane;
    generate for (lane=0; lane<LANES; lane=lane+1) begin : mask_tail
        wire valid_lane = element_base + lane < {19'd0,run_k};
        assign core_weight[4*lane +: 4] = valid_lane ? selected_weight[4*lane +: 4] : 4'd0;
        assign core_activation[8*lane +: 8] = valid_lane ? selected_act[8*lane +: 8] : 8'd0;
    end endgenerate
    reg [29:0] scale_address;
    reg [255:0] scale_cache;
    reg [24:0] scale_cache_tag;
    reg scale_cache_valid;
    wire [15:0] selected_scale;
    reg reduce_input_valid;reg signed [31:0] reduce_partial_q;
    reg reduce_first_q,reduce_last_q;
    reg [29:0] row_scale_address;
    reg [4:0] scale_load_index;
    reg [5:0] tile_scale_sent,tile_scale_received,tile_scale_pending;
    reg tile_scale_valid,tile_scale_error;
    reg [29:0] tile_scale_return_address;
    wire tile_scale_loading=state==TILE_SCALE_LOAD||state==TILE_SCALE_DRAIN;
    wire tile_scale_accept=tile_scale_loading&&tile_scale_valid&&mem_req_ready;
    wire tile_scale_return=tile_scale_loading&&tile_scale_pending!=0&&mem_rsp_valid;
    wire [6:0] unused_scale_row_extent={3'd0,row_scale_address[4:1]}+{1'b0,run_groups}-7'd1;
    wire [15:0] tile_rows_remaining=run_n-row_idx;
    wire [8:0] tile_scale_last_item=tile_scale_items+{3'd0,run_groups}-9'd1;
    wire [8:0] tile_scale_read={1'b0,tile_row_offsets[core_row[2:0]]}+{3'd0,core_group[5:0]};
    wire [8:0] scale_read_index=tile_stream?tile_scale_read:{5'd0,row_scale_address[4:1]}+{3'd0,core_group[5:0]};
    wire scale_load_last=tile_stream?scale_load_index==tile_last_scale_line:scale_load_index=={3'd0,unused_scale_row_extent[5:4]};
    reg [31:0] drain_error;
    reg [63:0] scale_end;
    wire scale_hit=scale_cache_valid && scale_cache_tag==scale_address[29:5];
    wire scale_request=state==SCALE_REQ && !scale_hit && !prefetch_valid && weight_pending==0;
    reg fused_clear;
    always @(posedge clk or posedge rst)
        if(rst)fused_clear<=0;
        else fused_clear<=req_valid&&req_ready&&req_write&&req_addr==CSR_BASE&&
                          req_wstrb==4'hf&&!busy&&(req_wdata==2||(req_wdata==1&&!done&&!error));
    wire reduce_ready,unused_reduce_done,reduce_row_done,reduce_invalid;
    wire signed [31:0] reduce_row;
    wire [31:0] numeric_flags,saturation_count,underflow_count,invalid_count;
    wire core_accept = busy && (tile_stream ? tile_running && reduce_ready : fused ? (state==RESULT || state==WEIGHT_REQ || state==ISSUE || state==ACT_FETCH || state==ACT_FETCH_HIGH) && reduce_ready :
                              (streaming ? pack_count<4 && state!=DRAIN : state==RESULT));
    wire scale_fill_cache=(state==SCALE_REQ||state==TILE_SCALE_SETUP)&&scale_hit&&!prefetch_valid&&weight_pending==0;
    wire scale_fill_response=((state==SCALE_RSP&&mem_rsp_valid&&mem_rsp_ready)||tile_scale_return)&&!mem_rsp_error;
    wire [4:0] scale_write_index=tile_scale_loading?tile_scale_received[4:0]:scale_load_index;
    wire scale_fill=scale_fill_cache||scale_fill_response;
    wire [255:0] scale_fill_data=scale_fill_cache?scale_cache:mem_rsp_data;
    reg [3:0] scale_lane_q;
    wire [255:0] scale_bank_q;
    genvar scale_lane;
    generate for(scale_lane=0;scale_lane<16;scale_lane=scale_lane+1)begin: scale_banks
        reg [15:0] coefficients[0:(TILE_CAPABLE?16:2)];
        reg [15:0] coefficient_q;
        always @(posedge clk)begin
            if(scale_fill)coefficients[scale_write_index]<=scale_fill_data[scale_lane*16+:16];
            if(fused&&core_out_valid&&core_accept)coefficient_q<=coefficients[scale_read_index[8:4]];
        end
        assign scale_bank_q[scale_lane*16+:16]=coefficient_q;
    end endgenerate
    assign selected_scale=scale_bank_q[scale_lane_q*16+:16];
    always @(posedge clk or posedge rst)begin
        if(rst)begin reduce_input_valid<=0;reduce_partial_q<=0;reduce_first_q<=0;reduce_last_q<=0;scale_lane_q<=0;end
        else if(fused_clear||state==DRAIN)reduce_input_valid<=0;
        else begin
            reduce_input_valid<=fused&&core_out_valid&&core_accept;
            if(fused&&core_out_valid&&core_accept)begin
                reduce_partial_q<=core_sum;reduce_first_q<=core_group==0;
                reduce_last_q<=core_group=={10'd0,run_groups}-1;scale_lane_q<=scale_read_index[3:0];
            end
        end
    end
    npu_w4_fused_reduce fused_reduce(
        .clk(clk),.rst(rst),.clear(fused_clear),
        .in_valid(reduce_input_valid&&state!=DRAIN),.in_ready(reduce_ready),
        .partial_sum(reduce_partial_q),.scale_bits(selected_scale),
        .maximum(cfg_maximum),.first_group(reduce_first_q),.last_group(reduce_last_q),
        .term_done(unused_reduce_done),.row_done(reduce_row_done),.invalid_scale(reduce_invalid),.row_result(reduce_row),
        .numeric_flags(numeric_flags),.saturation_count(saturation_count),
        .underflow_count(underflow_count),.invalid_count(invalid_count));
    wire core_ready, core_out_valid;
    wire signed [31:0] core_sum;
    wire [15:0] core_row, core_group;
    generate if (PIPELINED_CORE && LANES==16) begin : pipelined_compute
        gemv_pipeline16 core (
            .clk(clk), .rst_n(~rst && state!=DRAIN), .in_valid(issue_valid), .in_ready(core_ready),
            .weight_data(core_weight), .activation_data(core_activation),
            .group_first(issue_beat==0), .group_last(issue_beat==LAST_BEAT),
            .row_id(issue_row), .group_id({10'd0,issue_group}),
            .out_valid(core_out_valid), .out_ready(core_accept),
            .partial_sum(core_sum), .out_row_id(core_row), .out_group_id(core_group)
        );
    end else if(PIPELINED_CORE) begin : wide_pipelined_compute
        gemv_pipeline_v2 #(.LANES(LANES)) core (
            .clk(clk), .rst_n(~rst && state!=DRAIN), .in_valid(issue_valid), .in_ready(core_ready),
            .weight_data(core_weight), .activation_data(core_activation),
            .group_first(issue_beat==0), .group_last(issue_beat==LAST_BEAT),
            .row_id(issue_row), .group_id({10'd0,issue_group}),
            .out_valid(core_out_valid), .out_ready(core_accept),
            .partial_sum(core_sum), .out_row_id(core_row), .out_group_id(core_group)
        );
    end else begin : original_compute
        gemv_core #(.LANES(LANES)) core (
            .clk(clk), .rst_n(~rst && state!=DRAIN), .in_valid(issue_valid), .in_ready(core_ready),
            .weight_data(core_weight), .activation_data(core_activation),
            .group_first(issue_beat==0), .group_last(issue_beat==LAST_BEAT),
            .row_id(issue_row), .group_id({10'd0,issue_group}),
            .out_valid(core_out_valid), .out_ready(core_accept),
            .partial_sum(core_sum), .out_row_id(core_row), .out_group_id(core_group)
        );
    end endgenerate
    // Responses have no external ID and must remain in request order. The
    // allocation/return/consume pointers are internal tags for four reserved slots.
    localparam integer WPTR=$clog2(WEIGHT_DEPTH);
    reg [255:0] weight_fifo [0:WEIGHT_DEPTH-1] /* synthesis syn_ramstyle = "select_ram" */;
    reg [WEIGHT_DEPTH-1:0] weight_ready, weight_fault;
    reg [WPTR-1:0] weight_alloc, weight_return, weight_consume;
    reg [WPTR:0] weight_count, weight_pending;
    reg prefetch_active, prefetch_end, prefetch_stop, prefetch_valid;
    reg [29:0] prefetch_addr, prefetch_next, prefetch_row_base;
    reg [15:0] prefetch_row;
    reg [5:0] prefetch_group;
    reg prefetch_half;
    wire prefetch_hs = mem_req_valid && mem_req_ready && prefetch_valid;
    wire weight_rsp_hs = mem_rsp_valid && mem_rsp_ready && weight_pending!=0;
    wire weight_pop = weight_ready[weight_consume] &&
        (tile_stream ? state==TILE_RUN&&row_idx<tile_end_row&&tile_input_ready : state==WEIGHT_REQ);
    wire output_request = state==OUT_REQ && !prefetch_valid && weight_pending==0;
    assign mem_req_valid = !rst && busy &&
        (state==ACT_REQ || prefetch_valid || output_request || scale_request || (tile_scale_loading&&tile_scale_valid));
    assign mem_req_write = output_request;
    assign mem_req_addr = state==ACT_REQ ? activation_address :
                          prefetch_valid ? prefetch_addr : (scale_request||tile_scale_loading) ? {scale_address[29:5],5'd0} : pack_addr;
    // Zero read payloads stay stable even while the result packer advances.
    assign mem_req_wdata = mem_req_write ? pack_data : 256'd0;
    assign mem_req_wstrb = mem_req_write ? pack_strobe : 32'd0;
    assign mem_rsp_ready = !rst && busy &&
        (state==ACT_RSP || weight_pending!=0 || state==OUT_RSP || state==SCALE_RSP || (tile_scale_loading&&tile_scale_pending!=0));
    assign req_ready = !rst && !rsp_valid;

    always @(posedge clk or posedge rst) begin
        if (rst) begin
            rsp_valid<=0; rsp_rdata<=0; rsp_error<=0;
            cfg_n<=0; cfg_k<=0; cfg_weight<=0; cfg_activation<=0; cfg_output<=0;
            cfg_weight_hi<=0; cfg_activation_hi<=0; cfg_output_hi<=0;
            cfg_weight_stride<=0; cfg_activation_stride<=0; cfg_output_stride<=0;
            cfg_mode<=0;cfg_scale<=0;cfg_maximum<=0;scale_address<=0;scale_cache<=0;
            scale_cache_tag<=0;scale_cache_valid<=0;row_scale_address<=0;scale_load_index<=0;drain_error<=WEIGHT_FAULT;
            scale_end<=0;tile_scale_sent<=0;tile_scale_received<=0;tile_scale_pending<=0;tile_scale_valid<=0;tile_scale_error<=0;tile_scale_return_address<=0;
            state<=IDLE; busy<=0; done<=0; error<=0; error_code<=0;
            tile_input_valid<=0;tile_input_row<=0;tile_input_group<=0;tile_input_beat<=0;
            tile_end_row<=0;tile_rows<=0;tile_plan_row<=0;tile_scale_items<=0;tile_scale_bytes<=0;tile_last_scale_line<=0;
            cycle_count<=0; result_count<=0;
            run_n<=0; run_k<=0; run_groups<=0; row_idx<=0; group_idx<=0;
            beat_idx<=0; act_lines<=0; fill_idx<=0;
            activation_address<=0; row_output_address<=0;
            group_output_address<=0;
            weight_stride<=0; activation_stride<=0; output_stride<=0;
            weight_line<=0; pack_data<=0; pack_strobe<=0;
            pack_addr<=0; pack_count<=0; issued_batch<=0; last_result<=0;
            validation_weight<=0;validation_activation<=0;validation_output<=0;
            w_end<=0;a_end<=0;o_end<=0;validation_product<=0;validation_term<=0;
            validation_multiplier<=0;validation_kind<=0;
            weight_ready<=0; weight_fault<=0; weight_alloc<=0;
            weight_return<=0; weight_consume<=0; weight_count<=0; weight_pending<=0;
            prefetch_active<=0; prefetch_end<=0; prefetch_stop<=0; prefetch_valid<=0;
            prefetch_addr<=0; prefetch_next<=0; prefetch_row_base<=0;
            prefetch_row<=0; prefetch_group<=0; prefetch_half<=0;
        end else begin
            if(tile_scale_loading)begin
                case({tile_scale_accept,tile_scale_return})
                    2'b10:tile_scale_pending<=tile_scale_pending+6'd1;
                    2'b01:tile_scale_pending<=tile_scale_pending-6'd1;
                    default:begin end
                endcase
                if(!tile_scale_valid&&state==TILE_SCALE_LOAD&&!tile_scale_error&&tile_scale_sent<({1'b0,tile_last_scale_line}+6'd1)&&tile_scale_pending<WEIGHT_DEPTH)tile_scale_valid<=1;
                if(tile_scale_accept)begin
                    tile_scale_sent<=tile_scale_sent+6'd1;scale_address<=scale_address+30'd32;
                    tile_scale_valid<=state==TILE_SCALE_LOAD&&!tile_scale_error&&!(tile_scale_return&&mem_rsp_error)&&tile_scale_sent<{1'b0,tile_last_scale_line}&&(tile_scale_pending<WEIGHT_DEPTH-1||tile_scale_return);
                end
                if(tile_scale_return)begin
                    tile_scale_received<=tile_scale_received+6'd1;tile_scale_return_address<=tile_scale_return_address+30'd32;
                    if(mem_rsp_error)tile_scale_error<=1;
                    else begin scale_cache<=mem_rsp_data;scale_cache_tag<=tile_scale_return_address[29:5];scale_cache_valid<=1;end
                end
            end
            if(tile_stream&&tile_running&&tile_input_ready)begin
                tile_input_valid<=weight_pop&&!weight_fault[weight_consume];
                if(weight_pop)begin tile_input_row<=row_idx;tile_input_group<=group_idx;tile_input_beat<=beat_idx;end
            end
            if(state==DRAIN||!busy)tile_input_valid<=0;
            // Registered offers isolate DDR ready from the address scheduler.
            // A held offer is completed even if an older read reports an error.
            if (prefetch_active && !prefetch_end && !prefetch_stop &&
                !prefetch_valid && weight_count<WEIGHT_DEPTH &&
                state!=OUT_REQ && state!=OUT_RSP && state!=DRAIN &&
                state!=SCALE_REQ && state!=SCALE_RSP && state!=TILE_SCALE_SETUP && !tile_scale_loading) begin
                prefetch_valid<=1; prefetch_addr<=prefetch_next;
            end
            if (prefetch_hs) begin
                // Refill the registered offer directly on acceptance. Reserve
                // a FIFO slot before asserting it; a held offer is never withdrawn.
                prefetch_valid<=tile_stream&&!prefetch_stop&&
                    !(weight_rsp_hs&&mem_rsp_error)&&
                    !(prefetch_half&&prefetch_group==run_groups-6'd1&&prefetch_row==run_n-16'd1)&&
                    (weight_count<WEIGHT_DEPTH-1||weight_pop)&&state!=OUT_REQ&&state!=OUT_RSP&&state!=DRAIN;
                prefetch_addr<=prefetch_half&&prefetch_group==run_groups-6'd1 ? 
                    prefetch_row_base+weight_stride:prefetch_next+30'd32;
                weight_alloc<=weight_alloc+1'b1;
                weight_ready[weight_alloc]<=0; weight_fault[weight_alloc]<=0;
                prefetch_half<=!prefetch_half;
                if (!prefetch_half) prefetch_next<=prefetch_next+30'd32;
                else if (prefetch_group==run_groups-6'd1) begin
                    prefetch_group<=0; prefetch_row<=prefetch_row+16'd1;
                    prefetch_row_base<=prefetch_row_base+weight_stride;
                    prefetch_next<=prefetch_row_base+weight_stride;
                    if (prefetch_row==run_n-16'd1) prefetch_end<=1;
                end else begin
                    prefetch_group<=prefetch_group+6'd1;
                    prefetch_next<=prefetch_next+30'd32;
                end
            end
            case ({prefetch_hs,weight_pop})
                2'b10: weight_count<=weight_count+1'b1;
                2'b01: weight_count<=weight_count-1'b1;
                default: begin end
            endcase
            case ({prefetch_hs,weight_rsp_hs})
                2'b10: weight_pending<=weight_pending+1'b1;
                2'b01: weight_pending<=weight_pending-1'b1;
                default: begin end
            endcase
            if (weight_rsp_hs) begin
                weight_fifo[weight_return]<=mem_rsp_data;
                weight_ready[weight_return]<=1;
                weight_fault[weight_return]<=mem_rsp_error;
                weight_return<=weight_return+1'b1;
                if (mem_rsp_error) prefetch_stop<=1;
            end
            if (weight_pop) begin
                weight_ready[weight_consume]<=0;
                weight_consume<=weight_consume+1'b1;
            end
            // The elastic core can retire an older group while the next group issues.
            // Bound each batch to four records and one row, preserving write-ACK commits.
            if(streaming && busy && state!=DRAIN && core_out_valid && pack_count<4)begin
                pack_data[pack_count*64+:64]<={core_sum,core_group,core_row};
                pack_strobe[pack_count*8+:8]<=8'hff;
                if(pack_count==0)pack_addr<=group_output_address;
                pack_count<=pack_count+3'd1;
                last_result<=core_row==run_n-1 && core_group==({10'd0,run_groups}-16'd1);
                if(core_group==({10'd0,run_groups}-16'd1))begin
                    row_idx<=row_idx+1;row_output_address<=row_output_address+output_stride;
                    group_output_address<=row_output_address+output_stride;
                end else group_output_address<=group_output_address+8;
            end
            if(tile_running&&reduce_row_done)begin
                pack_data[pack_count*32+:32]<=reduce_row;
                pack_strobe[pack_count*4+:4]<=4'hf;
                if(pack_count==0)pack_addr<={group_output_address[29:5],5'd0};
                group_output_address<=group_output_address+30'd4;
                pack_count<=pack_count+4'd1;
                last_result<=tile_end_row==run_n;
                if(pack_count+4'd1==tile_rows)begin
                    row_scale_address<=row_scale_address+{20'd0,tile_scale_bytes};
                    state<=OUT_REQ;
                end
            end
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
                                    run_n<=cfg_n[15:0]; run_k<=cfg_k[12:0];tile_input_valid<=0;
                                    run_groups<=cfg_groups[5:0];
                                    scale_address<=cfg_scale[29:0];row_scale_address<=cfg_scale[29:0];
                                    scale_load_index<=0;scale_cache_valid<=0;
                                    drain_error<=WEIGHT_FAULT;
                                    prefetch_next<=cfg_weight[29:0]; prefetch_row_base<=cfg_weight[29:0];
                                    prefetch_row<=0; prefetch_group<=0; prefetch_half<=0;
                                    prefetch_active<=0; prefetch_end<=0; prefetch_stop<=0; prefetch_valid<=0;
                                    weight_ready<=0; weight_fault<=0; weight_alloc<=0; weight_return<=0;
                                    weight_consume<=0; weight_count<=0; weight_pending<=0;
                                    row_output_address<=cfg_output[29:0]; group_output_address<=cfg_output[29:0];
                                    activation_address<=cfg_activation[29:0]; weight_stride<=cfg_wstride[29:0];
                                    activation_stride<=cfg_astride[29:0]; output_stride<=cfg_ostride[29:0];
                                    act_lines<=cfg_k[12:5] + {7'd0,(|cfg_k[4:0])}; fill_idx<=0;
                                    row_idx<=0; group_idx<=0; beat_idx<=0;
                                    pack_count<=0; issued_batch<=0; pack_data<=0; pack_strobe<=0;
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
                        12'h04c: if (!busy) cfg_mode<=req_wdata; else rsp_error<=1;
                        12'h050: if (!busy) cfg_scale<=req_wdata; else rsp_error<=1;
                        12'h054: if (!busy) cfg_maximum<=req_wdata; else rsp_error<=1;
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
                    12'h04c: rsp_rdata<=cfg_mode;
                    12'h050: rsp_rdata<=cfg_scale;
                    12'h054: rsp_rdata<=cfg_maximum;
                    12'h058: rsp_rdata<=numeric_flags;
                    12'h05c: rsp_rdata<=saturation_count;
                    12'h060: rsp_rdata<=underflow_count;
                    12'h064: rsp_rdata<=invalid_count;
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
                    end else if (validation_kind==2) begin
                        o_end<=o_begin+validation_product+(fused?64'd4:({58'd0,run_groups}<<3));
                        if(fused)begin
                            validation_kind<=3;validation_term<={57'd0,run_groups,1'b0};
                            validation_multiplier<=run_n;state<=VALIDATE_MUL;
                        end else state<=VALIDATE_FINAL;
                    end else begin
                        scale_end<={32'd0,cfg_scale}+validation_product;
                        state<=VALIDATE_FINAL;
                    end
                end
                VALIDATE_FINAL: begin
                    if (w_end>64'h4000_0000 || a_end>64'h4000_0000 || o_end>64'h4000_0000 ||
                        (fused && scale_end>64'h4000_0000)) begin
                        state<=IDLE;busy<=0;error<=1;error_code<=BAD_RANGE;
                    end else if (overlap_wa || overlap_wo || overlap_ao ||
                        (fused && ({32'd0,cfg_scale}<o_end && o_begin<scale_end))) begin
                        state<=IDLE;busy<=0;error<=1;error_code<=BAD_OVERLAP;
                    end else state<=ACT_REQ;
                end
                ACT_REQ: if (mem_req_valid && mem_req_ready) state<=ACT_RSP;
                ACT_RSP: if (mem_rsp_valid && mem_rsp_ready) begin
                    if (mem_rsp_error) begin
                        state<=IDLE; busy<=0; error<=1; error_code<=ACT_FAULT;
                    end else begin
                        if (fill_idx+8'd1==act_lines) begin
                            beat_idx<=0; prefetch_active<=1; state<=tile_stream?TILE_SETUP:fused?SCALE_REQ:WEIGHT_REQ;
                        end else begin fill_idx<=fill_idx+8'd1; activation_address<=activation_address+activation_stride; state<=ACT_REQ; end
                    end
                end
                WEIGHT_REQ: if (weight_pop) begin
                    if (weight_fault[weight_consume]) begin
                        prefetch_stop<=1; drain_error<=WEIGHT_FAULT;state<=DRAIN;
                    end else begin weight_line<=weight_fifo[weight_consume]; state<=WIDE_ACT?ISSUE:ACT_FETCH; end
                end
                // Report at the original consumption boundary, after draining
                // accepted reads and any held offer; CLEAR cannot race responses.
                DRAIN: if (weight_pending==0 && !prefetch_valid) begin
                    state<=IDLE; busy<=0; error<=1; error_code<=drain_error;
                    prefetch_active<=0; weight_ready<=0; weight_count<=0;
                end
                ACT_FETCH: begin
                    state<=LANES==64?ACT_FETCH_HIGH:ISSUE;
                end
                ACT_FETCH_HIGH: state<=ISSUE;
                ISSUE: if (core_ready) begin
                    if (beat_idx==LAST_BEAT)begin
                        if(fused)begin
                            beat_idx<=0;
                            if(group_idx==run_groups-1)state<=RESULT;
                            else begin group_idx<=group_idx+1;state<=WEIGHT_REQ;end
                        end else if(streaming)begin
                            issued_batch<=issued_batch+3'd1;beat_idx<=0;
                            if(group_idx==run_groups-1)group_idx<=0;else group_idx<=group_idx+1;
                            if(issued_batch==3 || group_idx==run_groups-1)state<=RESULT;
                            else state<=WEIGHT_REQ;
                        end else state<=RESULT;
                    end
                    else if (weight_line_last) begin
                        beat_idx<=beat_idx+3'd1; state<=WEIGHT_REQ;
                    end else begin
                        beat_idx<=beat_idx+3'd1;
                        // Even/odd beats select the two halves of the same cached line.
                        // Advance only on core_ready; odd beats still fetch the next line.
                        state<=(LANES==16 && REUSE_ACT_LINE && !beat_idx[0]) ? ISSUE : ACT_FETCH;
                    end
                end
                SCALE_REQ: if(scale_hit && !prefetch_valid && weight_pending==0)begin
                    
                    if(scale_load_last)state<=tile_stream?TILE_RUN:WEIGHT_REQ;
                    else begin scale_load_index<=scale_load_index+1;scale_address<=scale_address+32;end
                end else if(scale_request && mem_req_valid && mem_req_ready)state<=SCALE_RSP;
                SCALE_RSP:if(mem_rsp_valid && mem_rsp_ready)begin
                    if(mem_rsp_error)begin
                        prefetch_stop<=1;drain_error<=SCALE_FAULT;state<=DRAIN;
                    end else begin
                        scale_cache<=mem_rsp_data;scale_cache_tag<=scale_address[29:5];
                        scale_cache_valid<=1;
                        if(scale_load_last)state<=tile_stream?TILE_RUN:WEIGHT_REQ;
                        else begin scale_load_index<=scale_load_index+1;scale_address<=scale_address+32;state<=SCALE_REQ;end
                    end
                end
                TILE_SETUP: begin
                    tile_rows<=tile_rows_remaining>=8?4'd8:tile_rows_remaining[3:0];
                    tile_end_row<=run_n-row_idx>=8?row_idx+16'd8:run_n;
                    tile_plan_row<=0;tile_scale_items<={5'd0,row_scale_address[4:1]};
                    tile_scale_bytes<=0;scale_load_index<=0;scale_address<=row_scale_address;
                    state<=TILE_PLAN;
                end
                TILE_PLAN:begin
                    tile_row_offsets[tile_plan_row]<=tile_scale_items[7:0];
                    tile_scale_items<=tile_scale_items+{3'd0,run_groups};
                    tile_scale_bytes<=tile_scale_bytes+{3'd0,run_groups,1'b0};
                    if({1'b0,tile_plan_row}+4'd1==tile_rows)begin
                        tile_last_scale_line<=tile_scale_last_item[8:4];
                        state<=TILE_SCALE_SETUP;
                    end else tile_plan_row<=tile_plan_row+3'd1;
                end
                TILE_SCALE_SETUP:if(!prefetch_valid&&weight_pending==0)begin
                    tile_scale_sent<=scale_hit?6'd1:6'd0;tile_scale_received<=scale_hit?6'd1:6'd0;tile_scale_pending<=0;
                    tile_scale_valid<=0;tile_scale_error<=0;state<=TILE_SCALE_LOAD;
                    tile_scale_return_address<={scale_address[29:5],5'd0}+(scale_hit?30'd32:30'd0);
                    if(scale_hit)scale_address<=scale_address+30'd32;
                end
                TILE_SCALE_LOAD:begin
                    if(tile_scale_return&&mem_rsp_error)begin prefetch_stop<=1;state<=TILE_SCALE_DRAIN;end
                    else if(tile_scale_received=={1'b0,tile_last_scale_line}+6'd1)state<=TILE_RUN;
                end
                TILE_SCALE_DRAIN:if(tile_scale_pending==0&&!tile_scale_valid)begin
                    state<=IDLE;busy<=0;error<=1;error_code<=SCALE_FAULT;prefetch_active<=0;
                end
                TILE_RUN:if(weight_pop)begin
                    if(weight_fault[weight_consume])begin
                        prefetch_stop<=1;drain_error<=WEIGHT_FAULT;state<=DRAIN;
                    end else begin
                        weight_line<=weight_fifo[weight_consume];
                        if(beat_idx==LAST_BEAT)begin
                            beat_idx<=0;
                            if(group_idx==run_groups-1)begin
                                group_idx<=0;row_idx<=row_idx+16'd1;
                                if(row_idx+16'd1==tile_end_row)state<=TILE_DRAIN;
                            end else group_idx<=group_idx+6'd1;
                        end else beat_idx<=beat_idx+3'd1;
                    end
                end
                TILE_DRAIN:begin end
                RESULT: if(fused)begin
                    if(reduce_row_done)begin
                        pack_data[pack_count*32+:32]<=reduce_row;
                        pack_strobe[pack_count*4+:4]<=4'hf;
                        if(pack_count==0)pack_addr<={group_output_address[29:5],5'd0};
                        pack_count<=pack_count+1;group_idx<=0;row_idx<=row_idx+1;
                        group_output_address<=group_output_address+4;
                        row_scale_address<=row_scale_address+{23'd0,run_groups,1'b0};
                        scale_address<=row_scale_address+{23'd0,run_groups,1'b0};scale_load_index<=0;
                        last_result<=row_idx==run_n-1;
                        if(pack_count==7 || row_idx==run_n-1)state<=OUT_REQ;
                        else state<=SCALE_REQ;
                    end
                end else if(streaming)begin
                    if(pack_count=={1'b0,issued_batch} && issued_batch!=0)state<=OUT_REQ;
                end else if (core_out_valid) begin
                    // The core's own tags, not reconstructed tags, go to DDR.
                    pack_data[pack_count*64 +: 64]<={core_sum,core_group,core_row};
                    pack_strobe[pack_count*8 +: 8]<=8'hff;
                    if (pack_count==0) pack_addr<=output_address[29:0];
                    pack_count<=pack_count+3'd1;
                    last_result <= row_idx==run_n-16'd1 && group_idx==run_groups-6'd1;
                    if (group_idx==run_groups-6'd1) begin
                        group_idx<=0; row_idx<=row_idx+16'd1;
                        row_output_address<=row_output_address+output_stride;
                        group_output_address<=row_output_address+output_stride;
                    end else begin
                        group_idx<=group_idx+6'd1;
                        group_output_address<=group_output_address+30'd8;
                    end
                    beat_idx<=0;
                    if (pack_count==3 || group_idx==run_groups-6'd1) state<=OUT_REQ;
                    else state<=WEIGHT_REQ;
                end
                OUT_REQ: if (output_request && mem_req_valid && mem_req_ready) state<=OUT_RSP;
                OUT_RSP: if (mem_rsp_valid && mem_rsp_ready) begin
                    if (mem_rsp_error) begin
                        state<=IDLE; busy<=0; error<=1; error_code<=WRITE_FAULT; prefetch_active<=0;
                    end else begin
                        result_count<=result_count+{28'd0,pack_count};
                        pack_count<=0; issued_batch<=0; pack_data<=0; pack_strobe<=0;
                        if (last_result) begin state<=IDLE; busy<=0; done<=1; prefetch_active<=0; end
                        else state<=tile_stream?TILE_SETUP:fused?SCALE_REQ:WEIGHT_REQ;
                    end
                end
                IDLE: begin end
                default: state<=IDLE;
            endcase
            if(fused && busy && reduce_invalid)begin
                prefetch_stop<=1;drain_error<=BAD_SCALE;state<=DRAIN;
            end
        end
    end
endmodule
