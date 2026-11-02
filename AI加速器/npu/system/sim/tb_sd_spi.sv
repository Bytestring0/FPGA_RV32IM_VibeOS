`timescale 1ns/1ps
module tb_sd_spi;
    logic clk=0, rst=1;
    always #5 clk = ~clk;
    logic req_valid=0, req_write=0, rsp_ready=1;
    logic [31:0] req_addr=0, req_wdata=0;
    logic [3:0] req_wstrb=0;
    logic req_ready, rsp_valid, rsp_error;
    logic [31:0] rsp_rdata;
    logic spi_clk, spi_mosi, spi_cs_n, spi_miso, card_present=1;

    sd_spi_top #(.CSR_BASE(32'h10005000)) dut (
        .clk, .rst, .req_valid, .req_ready, .req_write, .req_addr,
        .req_wdata, .req_wstrb, .rsp_valid, .rsp_ready, .rsp_rdata,
        .rsp_error, .spi_clk, .spi_mosi, .spi_cs_n, .spi_miso, .card_present
    );
    sd_spi_card_model card (.reset(rst), .spi_clk, .spi_cs_n, .spi_mosi, .spi_miso);

    task automatic bus_write(input [31:0] a, input [31:0] d);
        begin
            @(negedge clk); req_addr=a; req_wdata=d; req_write=1; req_wstrb=4'hf; req_valid=1;
            while (!req_ready) @(negedge clk);
            @(negedge clk); req_valid=0; req_write=0; req_wstrb=0;
            while (!rsp_valid) @(posedge clk);
            if (rsp_error) $fatal(1,"write error %h",a);
            @(posedge clk);
        end
    endtask
    task automatic bus_read(input [31:0] a, output [31:0] d);
        begin
            @(negedge clk); req_addr=a; req_wdata=0; req_write=0; req_wstrb=0; req_valid=1;
            while (!req_ready) @(negedge clk);
            @(negedge clk); req_valid=0;
            while (!rsp_valid) @(posedge clk);
            d=rsp_rdata;
            if (rsp_error) $fatal(1,"read error %h",a);
            @(posedge clk);
        end
    endtask
    task automatic xfer(input [7:0] tx, output [7:0] rx);
        reg [31:0] status;
        integer n;
        begin : xfer_wait
            bus_write(32'h10005010,{24'd0,tx});
            n=0;
            repeat (100000) begin
                bus_read(32'h10005004,status);
                if (!(status & 32'h1)) begin
                    bus_read(32'h10005014,status); rx=status[7:0]; disable xfer_wait;
                end
                n=n+1;
            end
            $fatal(1,"byte timeout");
        end
    endtask
    task automatic command(input [5:0] index, input [31:0] arg, output [7:0] r1);
        reg [7:0] p[0:5], b;
        integer i;
        reg [6:0] crc;
        reg fb;
        integer t;
        begin : command_wait
            bus_write(32'h10005000,0);
            bus_write(32'h10005000,1);
            p[0]=8'h40|index; p[1]=arg[31:24]; p[2]=arg[23:16];
            p[3]=arg[15:8]; p[4]=arg[7:0];
            crc=0;
            for(i=0;i<5;i=i+1) for(t=7;t>=0;t=t-1) begin
                fb=crc[6]^p[i][t]; crc={crc[5:0],1'b0};
                if(fb) crc=crc^7'h09;
            end
            p[5]={crc,1'b1};
            for(i=0;i<6;i=i+1) xfer(p[i],b);
            repeat(32) begin xfer(8'hff,b); if(!(b&8'h80)) begin r1=b; disable command_wait; end end
            $fatal(1,"command timeout %0d",index);
        end
    endtask
    task automatic read_sector(input integer lba);
        reg [7:0] r,b,crc_hi,crc_lo;
        reg [31:0] status,word;
        integer i,j,expected;
        begin
            command(17,lba,r); if(r!==0) $fatal(1,"CMD17 r1=%h",r);
            begin : token_wait
                repeat(32) begin xfer(8'hff,b); if(b==8'hfe) disable token_wait; end
                $fatal(1,"missing token");
            end
            bus_write(32'h10005028,512);
            begin : burst_wait
                repeat(100000) begin bus_read(32'h10005004,status); if(!(status&1)) disable burst_wait; end
                $fatal(1,"burst timeout");
            end
            bus_read(32'h10005018,status); if(status<512) $fatal(1,"burst count=%0d",status);
            for(i=0;i<128;i=i+1) begin
                bus_read(32'h10005024,word);
                for(j=0;j<4;j=j+1) begin
                    expected=(lba*37+(i*4+j)*13+((i*4+j)>>3))&255;
                    if(word[8*j +: 8]!==expected[7:0]) $fatal(1,"LBA %0d byte %0d got %h exp %h",lba,i*4+j,word[8*j +: 8],expected);
                end
            end
            xfer(8'hff,crc_hi); xfer(8'hff,crc_lo);
            bus_write(32'h10005000,0);
        end
    endtask
    initial begin
        reg [7:0] unused, r, b;
        reg [31:0] ocr;
        integer k;
        repeat(4) @(posedge clk); rst=0;
        bus_write(32'h10005000,2);
        bus_write(32'h10005008,1);
        repeat(10) xfer(8'hff,unused);
        command(0,0,r); if(r!==1) $fatal(1,"CMD0");
        command(8,32'h1aa,r); if(r!==1) $fatal(1,"CMD8");
        repeat(4) xfer(8'hff,b);
        command(55,0,r); command(41,32'h40000000,r); if(r!==0) $fatal(1,"ACMD41");
        command(58,0,r); for(k=0;k<4;k=k+1) begin xfer(8'hff,b); ocr={ocr[23:0],b}; end
        if(!ocr[31] || !ocr[30]) $fatal(1,"OCR=%h",ocr);
        read_sector(3); read_sector(7);
        $display("PASS sd SPI init, CMD17 LBA3/LBA7, 512-byte PIO, waveform BFM");
        $finish;
    end
endmodule

// Host wrapper used to execute the unmodified SD C algorithm against
// the same RTL and waveform BFM as the SV-only test. Faults are compile-time
// parameters, not backdoors in the DUT or the card payload.
module sd_spi_test_wrapper #(
    parameter integer SDHC=1, CORRUPT_CRC=0, STALL_READ=0, READY_AFTER_ACMD41=1,
    parameter integer FAULT_ONCE=1
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
    sd_spi_top dut(
        .clk,.rst,.card_present,.req_valid,.req_write,.req_addr,.req_wdata,
        .req_wstrb,.req_ready,.rsp_valid,.rsp_ready,.rsp_rdata,.rsp_error,
        .spi_clk,.spi_cs_n,.spi_mosi,.spi_miso
    );
    sd_spi_card_model #(.SDHC(SDHC),.CORRUPT_CRC(CORRUPT_CRC),
        .STALL_READ(STALL_READ),.READY_AFTER_ACMD41(READY_AFTER_ACMD41),.FAULT_ONCE(FAULT_ONCE)) card(
        .reset(rst),.spi_clk,.spi_cs_n,.spi_mosi,.spi_miso(miso_raw)
    );
endmodule
