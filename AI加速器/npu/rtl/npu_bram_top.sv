// Programmable W4A8/G128 prototype. Reuses unmodified GEMV; no Golden ROM.
module npu_bram_top #(
    parameter [31:0] BRAM_BYTES = 32'd32768,
    parameter [31:0] CSR_BASE = 32'h10001000,
    parameter [31:0] DATA_BASE = 32'h10010000
) (
    input wire clk, rst_n,
    input wire req_valid,
    output wire req_ready,
    input wire req_write,
    input wire [31:0] req_addr, req_wdata,
    input wire [3:0] req_wstrb,
    output reg rsp_valid,
    input wire rsp_ready,
    output wire [31:0] rsp_rdata,
    output reg rsp_error,
    output wire busy,
    output reg done,
    output wire error,
    output reg [31:0] error_code, result_count, cycle_count
);
    localparam integer AW = $clog2(BRAM_BYTES/4);
    localparam [31:0] BRAM_SIZE_U32 = BRAM_BYTES;
    localparam [63:0] BRAM_LIMIT = {32'd0,BRAM_BYTES};
    localparam [3:0] IDLE=0, READ_REQ=1, READ_CAPTURE=2, ISSUE=3,
                     WAIT_RESULT=4, WRITE_TAG=5, WRITE_VALUE=6;
    reg [3:0] state;
    reg [31:0] w_cfg, wh_cfg, a_cfg, ah_cfg, n_cfg, k_cfg, o_cfg;
    reg [31:0] task_w, task_a, task_o, task_n, task_k, task_padded, task_groups;
    reg [31:0] row_index, group_index, beat_index;
    reg [2:0] phase;
    reg [63:0] weight_hold;
    reg [127:0] activation_hold;
    reg [31:0] result_hold;
    reg [15:0] result_row_hold, result_group_hold;
    reg reader_done;
    reg [31:0] perf_fetch,perf_issue,perf_wait,perf_write,perf_beats,perf_macs;
    reg response_memory;
    reg [31:0] response_register;
    wire [31:0] a_rdata, b_rdata;
    wire [31:0] offset = req_addr-CSR_BASE;
    wire csr_select = req_addr>=CSR_BASE && req_addr<CSR_BASE+32'h100;
    wire mem_select = req_addr>=DATA_BASE && {1'b0,req_addr}<({1'b0,DATA_BASE}+{1'b0,BRAM_SIZE_U32});
    wire req_fire = req_valid && req_ready;
    assign req_ready = rst_n && (!rsp_valid || rsp_ready);
    assign rsp_rdata = response_memory ? a_rdata : response_register;
    assign busy = state!=IDLE;
    assign error = error_code!=0;
    reg register_known, register_writable;
    reg [31:0] register_value;
    always @* begin
        register_known=1; register_writable=0; register_value=0;
        case (offset)
            0: register_writable=1;
            4: register_value={27'd0,reader_done,1'b0,error,done,busy};
            8: begin register_value=w_cfg;register_writable=1;end
            12: begin register_value=wh_cfg;register_writable=1;end
            16: begin register_value=a_cfg;register_writable=1;end
            20: begin register_value=ah_cfg;register_writable=1;end
            24: begin register_value=n_cfg;register_writable=1;end
            28: begin register_value=k_cfg;register_writable=1;end
            32: register_value=error_code;
            36: register_value=cycle_count;
            40: register_value=result_count;
            44: begin register_value=o_cfg;register_writable=1;end
            48: register_value=32'h00001001; // 16 lanes and W4A8-partial capability, no IRQ.
            52: register_value=32'h4e505531;
            56: register_value=BRAM_BYTES;
            60: register_value=perf_fetch;
            64: register_value=perf_issue;
            68: register_value=perf_wait;
            72: register_value=perf_write;
            76: register_value=perf_beats;
            80: register_value=perf_macs;
            default: register_known=0;
        endcase
    end
    reg access_bad;
    reg [31:0] access_code;
    always @* begin
        access_bad=0;access_code=8;
        if (req_addr[1:0]!=0 || (req_write && req_wstrb==0)) access_bad=1;
        else if (mem_select) begin
            if (busy) begin access_bad=1;access_code=7;end
        end else if (csr_select && register_known) begin
            if (req_write && (!register_writable || req_wstrb!=4'hf)) access_bad=1;
            else if (req_write && offset!=0 && busy) begin access_bad=1;access_code=7;end
            else if (req_write && offset==0 && req_wdata!=1 && req_wdata!=2) begin access_bad=1;access_code=10;end
        end else access_bad=1;
    end
    wire a_en = req_fire && mem_select && !access_bad;
    wire [3:0] a_strobe = req_write ? req_wstrb : 4'd0;
    wire [31:0] unused_cpu_offset = req_addr-DATA_BASE;
    wire [AW-1:0] a_addr = unused_cpu_offset[AW+1:2];
    wire b_write = state==WRITE_TAG || state==WRITE_VALUE;
    wire b_en = rst_n && (state==READ_REQ || b_write);
    reg [31:0] b_byte_addr;
    always @* begin
        b_byte_addr=task_o+(result_count<<3);
        if (state==READ_REQ) begin
            if (phase<2) b_byte_addr=task_w+row_index*(task_padded>>1)+group_index*64+beat_index*8+{29'd0,phase}*4;
            else b_byte_addr=task_a+group_index*128+beat_index*16+({29'd0,phase}-2)*4;
        end else if (state==WRITE_VALUE) b_byte_addr=b_byte_addr+4;
    end
    wire [AW-1:0] b_addr = b_byte_addr[AW+1:2];
    wire [31:0] b_wdata = state==WRITE_TAG ? {result_group_hold,result_row_hold} : result_hold;
    npu_bram #(.WORDS(BRAM_BYTES/4)) memory (
        .clk(clk),.a_en(a_en),.a_addr(a_addr),.a_wdata(req_wdata),.a_wstrb(a_strobe),.a_rdata(a_rdata),
        .b_en(b_en),.b_we(b_write),.b_addr(b_addr),.b_wdata(b_wdata),.b_rdata(b_rdata)
    );

    // Wide arithmetic before truncation: no wrapping range/overlap acceptance.
    wire [63:0] padded_check=({32'd0,k_cfg}+127)&64'hffffffffffffff80;
    wire [63:0] w_bytes={32'd0,n_cfg}*(padded_check>>1);
    wire [63:0] a_bytes=padded_check;
    wire [63:0] o_bytes={32'd0,n_cfg}*(padded_check>>7)*8;
    wire [63:0] w_end={32'd0,w_cfg}+w_bytes;
    wire [63:0] a_end={32'd0,a_cfg}+a_bytes;
    wire [63:0] o_end={32'd0,o_cfg}+o_bytes;
    reg [31:0] config_error;
    always @* begin
        config_error=0;
        if (n_cfg==0 || n_cfg>64) config_error=1;
        else if (k_cfg==0 || k_cfg>4096) config_error=2;
        else if (w_cfg[3:0]!=0 || a_cfg[3:0]!=0 || o_cfg[1:0]!=0) config_error=4;
        else if (wh_cfg!=0 || ah_cfg!=0 || w_end>BRAM_LIMIT || a_end>BRAM_LIMIT || o_end>BRAM_LIMIT) config_error=3;
        else if (({32'd0,w_cfg}<a_end && {32'd0,a_cfg}<w_end) ||
                 ({32'd0,w_cfg}<o_end && {32'd0,o_cfg}<w_end) ||
                 ({32'd0,a_cfg}<o_end && {32'd0,o_cfg}<a_end)) config_error=5;
    end

    reg [63:0] core_weight;
    reg [127:0] core_activation;
    integer lane;
    always @* begin
        core_weight=0;core_activation=0;
        for (lane=0;lane<16;lane=lane+1)
            if (group_index*128+beat_index*16+lane<task_k) begin
                core_weight[4*lane +:4]=weight_hold[4*lane +:4];
                core_activation[8*lane +:8]=activation_hold[8*lane +:8];
            end
    end
    wire core_ready,core_valid;
    wire signed [31:0] core_partial;
    wire [15:0] core_row,core_group;
    wire tag_fault = state==WAIT_RESULT && core_valid &&
                     (core_row!=row_index[15:0] || core_group!=group_index[15:0]);
    wire [31:0] logical_left = task_k-(group_index*128+beat_index*16);
    wire [31:0] logical_lanes = group_index*128+beat_index*16>=task_k ? 0 :
                               (logical_left>=16 ? 16 : logical_left);
    gemv_core #(.LANES(16)) core (
        .clk(clk),.rst_n(rst_n),.in_valid(state==ISSUE),.in_ready(core_ready),
        .weight_data(core_weight),.activation_data(core_activation),
        .group_first(beat_index==0),.group_last(beat_index==7),
        .row_id(row_index[15:0]),.group_id(group_index[15:0]),
        .out_valid(core_valid),.out_ready(state==WAIT_RESULT),.partial_sum(core_partial),
        .out_row_id(core_row),.out_group_id(core_group)
    );

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state<=IDLE;done<=0;error_code<=0;result_count<=0;cycle_count<=0;reader_done<=0;
            perf_fetch<=0;perf_issue<=0;perf_wait<=0;perf_write<=0;perf_beats<=0;perf_macs<=0;
            rsp_valid<=0;rsp_error<=0;response_memory<=0;response_register<=0;
            w_cfg<=0;wh_cfg<=0;a_cfg<=0;ah_cfg<=0;n_cfg<=0;k_cfg<=0;o_cfg<=0;
            task_w<=0;task_a<=0;task_o<=0;task_n<=0;task_k<=0;task_padded<=0;task_groups<=0;
            row_index<=0;group_index<=0;beat_index<=0;phase<=0;weight_hold<=0;activation_hold<=0;
            result_hold<=0;result_row_hold<=0;result_group_hold<=0;
        end else begin
            if (rsp_valid && rsp_ready) rsp_valid<=0;
            if (busy) cycle_count<=cycle_count+1;
            case (state)
                READ_REQ,READ_CAPTURE: perf_fetch<=perf_fetch+1;
                ISSUE: begin
                    perf_issue<=perf_issue+1;
                    if (core_ready) begin perf_beats<=perf_beats+1;perf_macs<=perf_macs+logical_lanes;end
                end
                WAIT_RESULT: perf_wait<=perf_wait+1;
                WRITE_TAG,WRITE_VALUE: perf_write<=perf_write+1;
                default: begin end
            endcase
            case (state)
                READ_REQ: state<=READ_CAPTURE;
                READ_CAPTURE: begin
                    case (phase)
                        0: weight_hold[31:0]<=b_rdata;
                        1: weight_hold[63:32]<=b_rdata;
                        2: activation_hold[31:0]<=b_rdata;
                        3: activation_hold[63:32]<=b_rdata;
                        4: activation_hold[95:64]<=b_rdata;
                        5: activation_hold[127:96]<=b_rdata;
                        default: begin end
                    endcase
                    if (phase==5) state<=ISSUE;
                    else begin phase<=phase+1;state<=READ_REQ;end
                end
                ISSUE: if (core_ready) begin
                    if (beat_index==7) begin
                        state<=WAIT_RESULT;
                        if (row_index==task_n-1 && group_index==task_groups-1) reader_done<=1;
                    end else begin beat_index<=beat_index+1;phase<=0;state<=READ_REQ;end
                end
                WAIT_RESULT: if (core_valid) begin
                    if (tag_fault) begin
                        state<=IDLE;if (error_code==0) error_code<=9;done<=0;
                    end else begin
                        result_hold<=core_partial;result_row_hold<=core_row;result_group_hold<=core_group;state<=WRITE_TAG;
                    end
                end
                WRITE_TAG: state<=WRITE_VALUE;
                WRITE_VALUE: begin
                    result_count<=result_count+1;
                    if (row_index==task_n-1 && group_index==task_groups-1) begin state<=IDLE;done<=1;end
                    else begin
                        if (group_index==task_groups-1) begin row_index<=row_index+1;group_index<=0;end
                        else group_index<=group_index+1;
                        beat_index<=0;phase<=0;state<=READ_REQ;
                    end
                end
                default: begin end
            endcase
            if (req_fire) begin
                rsp_valid<=1;rsp_error<=access_bad;response_memory<=mem_select&&!access_bad;
                response_register<=register_value;
                if (access_bad) begin
                    if (error_code==0 && !tag_fault) error_code<=access_code;
                end else if (csr_select && req_write) begin
                    case (offset)
                        0: begin
                            if (busy) begin if (error_code==0 && !tag_fault) error_code<=6;end
                            else if (req_wdata==2) begin error_code<=0;done<=0;end
                            else if (!error) begin
                                done<=0;result_count<=0;cycle_count<=0;reader_done<=0;
                                perf_fetch<=0;perf_issue<=0;perf_wait<=0;perf_write<=0;perf_beats<=0;perf_macs<=0;
                                if (config_error!=0) error_code<=config_error;
                                else begin
                                    task_w<=w_cfg;task_a<=a_cfg;task_o<=o_cfg;task_n<=n_cfg;task_k<=k_cfg;
                                    task_padded<=padded_check[31:0];task_groups<=padded_check[38:7];
                                    row_index<=0;group_index<=0;beat_index<=0;phase<=0;state<=READ_REQ;
                                end
                            end
                        end
                        8: w_cfg<=req_wdata;
                        12: wh_cfg<=req_wdata;
                        16: a_cfg<=req_wdata;
                        20: ah_cfg<=req_wdata;
                        24: n_cfg<=req_wdata;
                        28: k_cfg<=req_wdata;
                        44: o_cfg<=req_wdata;
                        default: begin end
                    endcase
                end
            end
        end
    end
endmodule
