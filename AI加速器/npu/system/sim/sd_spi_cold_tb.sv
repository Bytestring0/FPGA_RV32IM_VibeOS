`timescale 1ns/1ps
// SD cold-load test wrapper; card data still crosses real SPI and MISO 2FF.
module sd_spi_test_wrapper #(
    parameter integer SDHC=1, CORRUPT_CRC=0, STALL_READ=0, READY_AFTER_ACMD41=1,
    parameter integer FAULT_ONCE=1, ENABLE_CRC16=1, REJECT_CMD18=0
) (
    input logic clk, rst, card_present,
    input logic req_valid, req_write, rsp_ready,
    input logic [31:0] req_addr, req_wdata,
    input logic [3:0] req_wstrb,
    output logic req_ready, rsp_valid, rsp_error,
    output logic [31:0] rsp_rdata,
    output logic spi_clk, spi_cs_n, spi_mosi
);
    logic spi_miso, miso_meta, miso_raw;
    always_ff @(posedge clk) begin
        if(rst) begin spi_miso<=1; miso_meta<=1; end
        else begin miso_meta<=miso_raw; spi_miso<=miso_meta; end
    end
    sd_spi_top #(.ENABLE_CRC16(ENABLE_CRC16 != 0)) dut(
        .clk,.rst,.card_present,.req_valid,.req_write,.req_addr,.req_wdata,
        .req_wstrb,.req_ready,.rsp_valid,.rsp_ready,.rsp_rdata,.rsp_error,
        .spi_clk,.spi_cs_n,.spi_mosi,.spi_miso
    );
    sd_spi_card_model #(.SDHC(SDHC),.CORRUPT_CRC(CORRUPT_CRC),
        .STALL_READ(STALL_READ),.REJECT_CMD18(REJECT_CMD18),.READY_AFTER_ACMD41(READY_AFTER_ACMD41),.FAULT_ONCE(FAULT_ONCE)) card(
        .reset(rst),.spi_clk,.spi_cs_n,.spi_mosi,.spi_miso(miso_raw)
    );
endmodule
