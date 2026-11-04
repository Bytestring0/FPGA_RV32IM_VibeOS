module sd_spi_top #(
    parameter [31:0] CSR_BASE = 32'h10005000,
    parameter bit ENABLE_CRC16 = 1'b1
) (
    input logic clk, rst,
    input logic req_valid, output logic req_ready,
    input logic req_write,
    input logic [31:0] req_addr, req_wdata,
    input logic [3:0] req_wstrb,
    output logic rsp_valid, input logic rsp_ready,
    output logic [31:0] rsp_rdata, output logic rsp_error,
    output logic spi_clk, spi_mosi, spi_cs_n,
    input logic spi_miso, card_present
);
    wire [31:0] offset=req_addr-CSR_BASE;
    wire selected=req_addr>=CSR_BASE&&req_addr<CSR_BASE+32'h1000;
    assign req_ready=!rst&&(!rsp_valid||rsp_ready);
    wire fire=req_valid&&req_ready;
    wire [7:0] fifo_lane[0:3];
    wire [31:0] fifo_word;
    wire [7:0] fifo_byte=fifo_lane[rd_ptr[1:0]];
    logic [9:0] wr_ptr,rd_ptr;
    logic [10:0] fifo_count;
    logic [9:0] burst_remaining;
    logic busy,cs_active;
    logic [7:0] tx_shift,rx_shift;
    logic [3:0] bit_count;
    logic [15:0] divider,divider_counter;
    logic [31:0] timeout_limit,elapsed;
    logic timeout_fault,card_fault,reject_fault;
    logic [15:0] burst_crc16;
    logic crc_active,crc_valid;
    localparam [2:0] SPACE=0,TOKEN=1,DATA=2,CRC_HI=3,CRC_LO=4;
    logic stream_active,stream_done,stream_crc_fault,stream_token_fault,stream_cancel;
    logic [2:0] stream_phase;
    logic [15:0] stream_remaining,stream_verified;
    logic [9:0] stream_bytes;
    logic [7:0] card_crc_hi;
    wire [15:0] half_period=divider==0?16'd1:divider;
    wire tick=busy&&divider_counter==half_period-1'b1;
    wire finish_byte=tick&&spi_clk&&bit_count==4'd8&&card_present&&elapsed<timeout_limit;
    wire [7:0] received_byte={rx_shift[6:0],spi_miso};
    wire stream_publish=finish_byte&&stream_active&&!stream_cancel&&!cancel_req&&stream_phase==CRC_LO&&
                            {card_crc_hi,received_byte}==burst_crc16;
    wire payload_push=finish_byte&&(!stream_active||stream_phase==DATA);
    wire legacy_push=payload_push&&!stream_active;
    logic bad;
    always_comb begin
        bad=!selected||req_addr[1:0]!=0||(req_write&&req_wstrb!=4'hf);
        if(!bad)begin
            case(offset)
                32'h00:if(req_write&&(req_wdata[31:3]!=0||
                    ((busy||stream_active)&&!req_wdata[2]&&(req_wdata[1]||req_wdata[0]!=cs_active))))bad=1;
                32'h04,32'h14,32'h18,32'h1c,32'h20,32'h24:if(req_write)bad=1;
                32'h2c:if(req_write||!ENABLE_CRC16)bad=1;
                32'h34,32'h38:if(req_write||!ENABLE_CRC16)bad=1;
                32'h08,32'h0c:if(req_write&&(busy||stream_active))bad=1;
                32'h10:if(!req_write||busy||stream_active||!card_present||fifo_count==1024)bad=1;
                32'h28:if(!req_write||busy||stream_active||!cs_active||!card_present||
                            fifo_count!=0||req_wdata==0||req_wdata>512)bad=1;
                32'h30:if(!req_write||!ENABLE_CRC16||req_wdata[31:16]!=0||
                    (req_wdata!=0&&(busy||stream_active||!cs_active||!card_present||fifo_count!=0)))bad=1;
                default:bad=1;
            endcase
            if(!req_write&&offset==32'h14&&fifo_count==0)bad=1;
            if(!req_write&&offset==32'h24&&fifo_count<4)bad=1;
        end
    end
    wire pop_byte=fire&&!bad&&!req_write&&offset==32'h14;
    wire pop_word=fire&&!bad&&!req_write&&offset==32'h24;
    wire flush=fire&&!bad&&req_write&&offset==0&&req_wdata[1];
    wire abort_req=fire&&!bad&&req_write&&offset==0&&req_wdata[2];
    wire cancel_req=fire&&!bad&&req_write&&offset==32'h30&&req_wdata==0;
    genvar lane;
    generate for(lane=0;lane<4;lane=lane+1)begin: receive_banks
        logic [7:0] bank[0:255];
        wire [7:0] read_index=rd_ptr[9:2]+{7'd0,(lane<rd_ptr[1:0])};
        assign fifo_lane[lane]=bank[read_index];
        always_ff @(posedge clk)
            if(!rst&&!flush&&!abort_req&&payload_push&&wr_ptr[1:0]==lane)
                bank[wr_ptr[9:2]]<=received_byte;
    end endgenerate
    assign fifo_word=rd_ptr[1:0]==0?{fifo_lane[3],fifo_lane[2],fifo_lane[1],fifo_lane[0]}:
                     rd_ptr[1:0]==1?{fifo_lane[0],fifo_lane[3],fifo_lane[2],fifo_lane[1]}:
                     rd_ptr[1:0]==2?{fifo_lane[1],fifo_lane[0],fifo_lane[3],fifo_lane[2]}:
                                      {fifo_lane[2],fifo_lane[1],fifo_lane[0],fifo_lane[3]};
    always_ff @(posedge clk)begin
        if(rst)begin
            rsp_valid<=0;rsp_error<=0;rsp_rdata<=0;
            wr_ptr<=0;rd_ptr<=0;fifo_count<=0;burst_remaining<=0;
            busy<=0;cs_active<=0;spi_cs_n<=1;spi_clk<=0;spi_mosi<=1;
            tx_shift<=8'hff;rx_shift<=0;bit_count<=0;
            divider<=4;divider_counter<=0;timeout_limit<=1000000;elapsed<=0;
            timeout_fault<=0;card_fault<=0;reject_fault<=0;
            burst_crc16<=0;crc_active<=0;crc_valid<=0;
            stream_active<=0;stream_done<=0;stream_crc_fault<=0;stream_token_fault<=0;
            stream_phase<=SPACE;stream_remaining<=0;stream_verified<=0;stream_bytes<=0;
            stream_cancel<=0;card_crc_hi<=0;
        end else begin
            if(rsp_valid&&rsp_ready)rsp_valid<=0;
            if(fire)begin
                rsp_valid<=1;rsp_error<=bad;rsp_rdata<=0;
                if(!bad&&!req_write)case(offset)
                    32'h00:rsp_rdata<={31'd0,cs_active};
                    32'h04:rsp_rdata<={5'd0,fifo_count,8'd0,reject_fault,(fifo_count>=512),card_fault,1'b0,
                                        timeout_fault,card_present,(fifo_count!=0),(busy||stream_active)};
                    32'h08:rsp_rdata<={16'd0,divider};
                    32'h0c:rsp_rdata<=timeout_limit;
                    32'h14:rsp_rdata<={24'd0,fifo_byte};
                    32'h18:rsp_rdata<={21'd0,fifo_count};
                    32'h1c:rsp_rdata<=32'h53445049;
                    32'h20:rsp_rdata<=ENABLE_CRC16?32'hf:32'h3;
                    32'h24:rsp_rdata<=fifo_word;
                    32'h2c:rsp_rdata<={15'd0,crc_valid,burst_crc16};
                    32'h34:rsp_rdata<={stream_remaining,12'd0,stream_token_fault,stream_crc_fault,stream_done,stream_active};
                    32'h38:rsp_rdata<={16'd0,stream_verified};
                    default:begin end
                endcase
                if(!bad&&req_write)case(offset)
                    32'h00:begin
                        if(req_wdata[2])begin
                            cs_active<=0;spi_cs_n<=1;spi_clk<=0;spi_mosi<=1;busy<=0;
                            burst_remaining<=0;elapsed<=0;timeout_fault<=0;card_fault<=0;reject_fault<=0;
                        end else begin cs_active<=req_wdata[0];spi_cs_n<=!req_wdata[0];end
                    end
                    32'h08:divider<=req_wdata[15:0]==0?16'd1:req_wdata[15:0];
                    32'h0c:timeout_limit<=req_wdata==0?32'hffffffff:req_wdata;
                    32'h10,32'h28:begin
                        busy<=1;spi_clk<=0;divider_counter<=0;elapsed<=0;rx_shift<=0;bit_count<=0;
                        burst_remaining<=offset==32'h28 ? req_wdata[9:0]:10'd1;
                        tx_shift<=offset==32'h28 ? 8'hff:req_wdata[7:0];
                        spi_mosi<=offset==32'h28 ? 1'b1:req_wdata[7];
                        if(ENABLE_CRC16&&offset==32'h28)begin burst_crc16<=0;crc_active<=1;crc_valid<=0;end
                    end
                    32'h30:begin
                        if(req_wdata!=0)begin
                            stream_active<=1;stream_done<=0;stream_crc_fault<=0;stream_token_fault<=0;
                            stream_cancel<=0;stream_phase<=SPACE;stream_remaining<=req_wdata[15:0];
                            stream_verified<=0;elapsed<=0;crc_valid<=0;
                        end else if(busy)stream_cancel<=1;
                        else begin stream_active<=0;stream_cancel<=0;crc_active<=0;end
                    end
                    default:begin end
                endcase
                if(bad&&req_write&&(offset==32'h10||offset==32'h28||offset==32'h30))reject_fault<=1;
            end
            if(flush||abort_req)begin
                fifo_count<=0;rd_ptr<=0;wr_ptr<=0;burst_crc16<=0;crc_active<=0;crc_valid<=0;
                stream_active<=0;stream_done<=0;stream_crc_fault<=0;stream_token_fault<=0;stream_cancel<=0;
                if(flush)begin timeout_fault<=0;card_fault<=0;reject_fault<=0;end
            end else if(!card_present&&(busy||cs_active))begin
                busy<=0;cs_active<=0;spi_cs_n<=1;spi_clk<=0;spi_mosi<=1;burst_remaining<=0;
                card_fault<=1;fifo_count<=0;wr_ptr<=0;rd_ptr<=0;crc_active<=0;crc_valid<=0;stream_active<=0;
            end else begin
                if(pop_byte)rd_ptr<=rd_ptr+10'd1;
                if(pop_word)rd_ptr<=rd_ptr+10'd4;
                if(payload_push)wr_ptr<=wr_ptr+10'd1;
                if(stream_publish)fifo_count<=fifo_count+11'd512-(pop_word?11'd4:pop_byte?11'd1:11'd0);
                else if(legacy_push)fifo_count<=fifo_count+11'd1-(pop_word?11'd4:pop_byte?11'd1:11'd0);
                else if(pop_word)fifo_count<=fifo_count-11'd4;
                else if(pop_byte)fifo_count<=fifo_count-11'd1;
                if(stream_active&&!busy&&!cancel_req)begin
                    if(stream_phase!=SPACE||fifo_count<=512)begin
                        busy<=1;spi_clk<=0;spi_mosi<=1;tx_shift<=8'hff;rx_shift<=0;bit_count<=0;
                        divider_counter<=0;burst_remaining<=1;
                        if(stream_phase==SPACE)begin stream_phase<=TOKEN;elapsed<=0;end
                    end
                end
                if(busy)begin
                    if(elapsed>=timeout_limit)begin
                        busy<=0;spi_clk<=0;spi_mosi<=1;spi_cs_n<=1;cs_active<=0;
                        burst_remaining<=0;timeout_fault<=1;crc_active<=0;crc_valid<=0;stream_active<=0;
                    end else begin
                        elapsed<=elapsed+1'b1;
                        if(tick)begin
                            divider_counter<=0;
                            if(!spi_clk)begin spi_clk<=1;bit_count<=bit_count+1'b1;end
                            else begin
                                spi_clk<=0;rx_shift<=received_byte;
                                if(ENABLE_CRC16&&crc_active)burst_crc16<={burst_crc16[14:0],1'b0}^
                                    ((burst_crc16[15]^spi_miso)?16'h1021:16'h0000);
                                if(bit_count==8)begin
                                    if(stream_active)begin
                                        busy<=0;spi_mosi<=1;
                                        if(stream_cancel||cancel_req)begin stream_active<=0;stream_cancel<=0;crc_active<=0;wr_ptr<=rd_ptr+fifo_count[9:0];end
                                        else case(stream_phase)
                                            TOKEN:begin
                                                if(received_byte==8'hfe)begin
                                                    stream_phase<=DATA;stream_bytes<=512;burst_crc16<=0;crc_active<=1;elapsed<=0;
                                                end else if(received_byte!=8'hff)begin stream_token_fault<=1;stream_active<=0;end
                                            end
                                            DATA:begin
                                                elapsed<=0;stream_bytes<=stream_bytes-1'b1;
                                                if(stream_bytes==1)begin stream_phase<=CRC_HI;crc_active<=0;end
                                            end
                                            CRC_HI:begin card_crc_hi<=received_byte;stream_phase<=CRC_LO;elapsed<=0;end
                                            CRC_LO:begin
                                                elapsed<=0;
                                                if({card_crc_hi,received_byte}!=burst_crc16)begin stream_crc_fault<=1;stream_active<=0;wr_ptr<=rd_ptr+fifo_count[9:0];end
                                                else begin
                                                    stream_verified<=stream_verified+1'b1;stream_remaining<=stream_remaining-1'b1;crc_valid<=1;
                                                    if(stream_remaining==1)begin stream_active<=0;stream_done<=1;end
                                                    else stream_phase<=SPACE;
                                                end
                                            end
                                            default:begin end
                                        endcase
                                    end else if(burst_remaining>1)begin
                                        burst_remaining<=burst_remaining-1'b1;bit_count<=0;rx_shift<=0;spi_mosi<=1;elapsed<=0;
                                    end else begin
                                        burst_remaining<=0;busy<=0;spi_mosi<=1;
                                        if(ENABLE_CRC16&&crc_active)begin crc_active<=0;crc_valid<=1;end
                                    end
                                end else spi_mosi<=tx_shift[7-bit_count];
                            end
                        end else divider_counter<=divider_counter+1'b1;
                    end
                end
            end
        end
    end
endmodule
