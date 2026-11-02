// SD SPI transport; CRC7 and card-CRC comparison live in the bare-metal driver.
// Full-word MMIO accesses only; one response per accepted request, backpressure safe.
module sd_spi_top #(
    parameter [31:0] CSR_BASE = 32'h10005000,
    parameter bit ENABLE_CRC16 = 1'b1
) (
    input  logic        clk, rst,
    input  logic        req_valid,
    output logic        req_ready,
    input  logic        req_write,
    input  logic [31:0] req_addr, req_wdata,
    input  logic [3:0]  req_wstrb,
    output logic        rsp_valid,
    input  logic        rsp_ready,
    output logic [31:0] rsp_rdata,
    output logic        rsp_error,
    output logic        spi_clk, spi_mosi, spi_cs_n,
    input  logic        spi_miso, card_present
);
    wire [31:0] offset = req_addr - CSR_BASE;
    wire selected = (req_addr >= CSR_BASE) && (req_addr < CSR_BASE + 32'h1000);
    assign req_ready = !rst && (!rsp_valid || rsp_ready);
    wire fire = req_valid && req_ready;

    logic [7:0] fifo [0:511];
    logic [8:0] wr_ptr, rd_ptr;
    logic [9:0] fifo_count, burst_remaining;
    logic busy, cs_active;
    logic [7:0] tx_shift, rx_shift;
    logic [3:0] bit_count;
    logic [15:0] divider, divider_counter;
    logic [31:0] timeout_limit, elapsed;
    logic timeout_fault, card_fault, reject_fault;
    logic [15:0] burst_crc16;
    logic crc_active, crc_valid;
    wire [15:0] half_period = (divider == 0) ? 16'd1 : divider;
    wire tick = busy && (divider_counter == half_period - 1'b1);
    wire finish_byte = tick && spi_clk && (bit_count == 4'd8) &&
                       card_present && (elapsed < timeout_limit);
    logic bad;
    always_comb begin
        bad = !selected || (req_addr[1:0] != 0) ||
              (req_write && req_wstrb != 4'hf);
        if (!bad) begin
            case (offset)
                32'h00: if (req_write && (req_wdata[31:3] != 0 ||
                    (busy && !req_wdata[2] && (req_wdata[1] || req_wdata[0] != cs_active)))) bad = 1;
                32'h04, 32'h14, 32'h18, 32'h1c, 32'h20, 32'h24:
                    if (req_write) bad = 1;
                32'h2c: if (req_write || !ENABLE_CRC16) bad = 1;
                32'h08, 32'h0c: if (req_write && busy) bad = 1;
                32'h10: if (!req_write || busy || !card_present ||
                                   fifo_count == 10'd512) bad = 1;
                32'h28: if (!req_write || busy || !cs_active || !card_present ||
                    fifo_count != 0 || req_wdata == 0 || req_wdata > 512) bad = 1;
                default: bad = 1;
            endcase
            if (!req_write && offset == 32'h14 && fifo_count == 0) bad = 1;
            if (!req_write && offset == 32'h24 && fifo_count < 4) bad = 1;
        end
    end
    wire pop_byte = fire && !bad && !req_write && offset == 32'h14;
    wire pop_word = fire && !bad && !req_write && offset == 32'h24;
    wire flush = fire && !bad && req_write && offset == 32'h00 && req_wdata[1];
    wire abort_req = fire && !bad && req_write && offset == 32'h00 && req_wdata[2];

    always_ff @(posedge clk) begin
        if (rst) begin
            rsp_valid <= 0; rsp_error <= 0; rsp_rdata <= 0;
            wr_ptr <= 0; rd_ptr <= 0; fifo_count <= 0; burst_remaining <= 0;
            busy <= 0; cs_active <= 0; spi_cs_n <= 1; spi_clk <= 0; spi_mosi <= 0;
            tx_shift <= 8'hff; rx_shift <= 0; bit_count <= 0;
            divider <= 16'd4; divider_counter <= 0;
            timeout_limit <= 32'd1000000; elapsed <= 0;
            timeout_fault <= 0; card_fault <= 0; reject_fault <= 0;
            burst_crc16 <= 0; crc_active <= 0; crc_valid <= 0;
        end else begin
            if (rsp_valid && rsp_ready) rsp_valid <= 0;
            if (fire) begin
                rsp_valid <= 1; rsp_error <= bad; rsp_rdata <= 0;
                if (!bad && !req_write) begin
                    case (offset)
                        32'h00: rsp_rdata <= {31'd0,cs_active};
                        32'h04: rsp_rdata <= {6'd0,fifo_count,8'd0,
                            reject_fault,(fifo_count == 10'd512),card_fault,1'b0,
                            timeout_fault,card_present,(fifo_count != 0),busy};
                        32'h08: rsp_rdata <= {16'd0,divider};
                        32'h0c: rsp_rdata <= timeout_limit;
                        32'h14: rsp_rdata <= {24'd0,fifo[rd_ptr]};
                        32'h18: rsp_rdata <= {22'd0,fifo_count};
                        32'h1c: rsp_rdata <= 32'h53445049;
                        32'h20: rsp_rdata <= ENABLE_CRC16 ? 32'h00000007 : 32'h00000003;
                        32'h2c: rsp_rdata <= {15'd0,crc_valid,burst_crc16};
                        32'h24: rsp_rdata <= {fifo[(rd_ptr + 9'd3) & 9'h1ff],
                            fifo[(rd_ptr + 9'd2) & 9'h1ff],
                            fifo[(rd_ptr + 9'd1) & 9'h1ff],fifo[rd_ptr]};
                        default: rsp_rdata <= 0;
                    endcase
                end
                if (!bad && req_write) begin
                    case (offset)
                        32'h00: begin
                            if (req_wdata[2]) begin
                                cs_active <= 0; spi_cs_n <= 1;
                                spi_clk <= 0; spi_mosi <= 0; busy <= 0;
                                burst_remaining <= 0; elapsed <= 0;
                                timeout_fault <= 0; card_fault <= 0; reject_fault <= 0;
                            end else begin
                                cs_active <= req_wdata[0];
                                spi_cs_n <= !req_wdata[0];
                            end
                        end
                        32'h08: divider <= (req_wdata[15:0] == 0) ? 16'd1 : req_wdata[15:0];
                        32'h0c: timeout_limit <= (req_wdata == 0) ? 32'hffffffff : req_wdata;
                        32'h10, 32'h28: begin
                            busy <= 1; spi_clk <= 0; divider_counter <= 0;
                            elapsed <= 0; rx_shift <= 0; bit_count <= 0;
                            burst_remaining <= (offset == 32'h28) ? req_wdata[9:0] : 10'd1;
                            tx_shift <= (offset == 32'h28) ? 8'hff : req_wdata[7:0];
                            spi_mosi <= (offset == 32'h28) ? 1'b1 : req_wdata[7];
                            if (ENABLE_CRC16 && offset == 32'h28) begin
                                burst_crc16 <= 0; crc_active <= 1; crc_valid <= 0;
                            end
                        end
                        default: begin end
                    endcase
                end
                if (bad && req_write && (offset == 32'h10 || offset == 32'h28))
                    reject_fault <= 1;
                if (bad && req_write && (offset == 32'h10 || offset == 32'h28) && !card_present)
                    card_fault <= 1;
            end
            if (flush || abort_req) begin
                fifo_count <= 0; rd_ptr <= 0; wr_ptr <= 0;
                burst_crc16 <= 0; crc_active <= 0; crc_valid <= 0;
                if (flush) begin
                    timeout_fault <= 0; card_fault <= 0; reject_fault <= 0;
                end
            end else if (!card_present && (busy || cs_active)) begin
                // Removal wins over a simultaneous receive, and releases the card lines.
                busy <= 0; cs_active <= 0; spi_cs_n <= 1; spi_clk <= 0;
                spi_mosi <= 0; burst_remaining <= 0; card_fault <= 1;
                fifo_count <= 0; wr_ptr <= 0; rd_ptr <= 0;
                burst_crc16 <= 0; crc_active <= 0; crc_valid <= 0;
            end else begin
                if (pop_byte) rd_ptr <= rd_ptr + 9'd1;
                if (pop_word) rd_ptr <= rd_ptr + 9'd4;
                if (finish_byte) wr_ptr <= wr_ptr + 9'd1;
                if (finish_byte && !pop_byte && !pop_word) fifo_count <= fifo_count + 10'd1;
                else if (finish_byte && pop_word) fifo_count <= fifo_count - 10'd3;
                else if (!finish_byte && pop_word) fifo_count <= fifo_count - 10'd4;
                else if (!finish_byte && pop_byte) fifo_count <= fifo_count - 10'd1;

                if (busy) begin
                    if (elapsed >= timeout_limit) begin
                        busy <= 0; spi_clk <= 0; spi_mosi <= 0;
                        spi_cs_n <= 1; cs_active <= 0;
                        burst_remaining <= 0; timeout_fault <= 1;
                        crc_active <= 0; crc_valid <= 0;
                    end else begin
                        elapsed <= elapsed + 32'd1;
                        if (tick) begin
                            divider_counter <= 0;
                            if (!spi_clk) begin
                                spi_clk <= 1;
                                rx_shift <= {rx_shift[6:0],spi_miso};
                                // CRC16/XMODEM, MSB first, seed 0. Only RXBURST
                                // payload clocks participate; command and trailer do not.
                                if (ENABLE_CRC16 && crc_active)
                                    burst_crc16 <= {burst_crc16[14:0],1'b0} ^
                                        ((burst_crc16[15] ^ spi_miso) ? 16'h1021 : 16'h0000);
                                bit_count <= bit_count + 4'd1;
                            end else begin
                                spi_clk <= 0;
                                if (bit_count == 4'd8) begin
                                    fifo[wr_ptr] <= rx_shift;
                                    if (burst_remaining > 10'd1) begin
                                        burst_remaining <= burst_remaining - 10'd1;
                                        bit_count <= 0; rx_shift <= 0;
                                        spi_mosi <= 1; elapsed <= 0;
                                    end else begin
                                        burst_remaining <= 0;
                                        if (ENABLE_CRC16 && crc_active) begin
                                            crc_active <= 0; crc_valid <= 1;
                                        end
                                        busy <= 0; spi_mosi <= 0;
                                    end
                                end else begin
                                    spi_mosi <= tx_shift[7-bit_count];
                                end
                            end
                        end else divider_counter <= divider_counter + 16'd1;
                    end
                end
            end
        end
    end
endmodule
