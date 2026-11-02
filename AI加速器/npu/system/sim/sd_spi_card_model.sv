`timescale 1ns/1ps
// SPI-mode SD waveform BFM: rising-edge MOSI capture, falling-edge MISO update.
// No host memory or expected-result backdoor reaches the DUT.
module sd_spi_card_model #(
    parameter integer SDHC = 1,
    parameter integer CORRUPT_CRC = 0,
    parameter integer STALL_READ = 0,
    parameter integer REJECT_CMD18 = 0,
    parameter integer READY_AFTER_ACMD41 = 1,
    parameter integer FAULT_ONCE = 0,
    parameter integer REQUIRE_POWERUP_CLOCKS = 74
) (
    input logic reset, spi_clk, spi_cs_n, spi_mosi,
    output logic spi_miso
);
    logic [7:0] command [0:5];
    logic [7:0] response [0:1023];
    integer response_head, response_tail;
    integer in_bit, out_bit, command_length;
    integer acmd41_count, read_count, powerup_clocks;
    integer multiblock, stream_stalled, stop_busy_bytes, stop_busy_remaining;
    reg [31:0] next_lba;
    logic [7:0] in_shift, out_shift;
    logic app_command, card_idle;
    // Optional external card image for system tests, mapped at LBA 16.
    // This is card storage only; no activation or expected-result backdoor.
    reg [7:0] card_image [0:65535];
    string image_file;
    integer image_bytes, have_image, image_index;
    reg [7:0] disk_image [0:2097151];
    string disk_file;
    integer disk_bytes,have_disk,disk_index;
    initial begin
        stop_busy_bytes=3;
        if($value$plusargs("SD_STOP_BUSY_BYTES=%d",stop_busy_bytes)) begin
            if(stop_busy_bytes < -1) $fatal(1,"invalid SD_STOP_BUSY_BYTES");
        end
    end
    initial begin
        disk_bytes=0;have_disk=$value$plusargs("SD_DISK_IMAGE=%s",disk_file);
        if(have_disk!=0) begin
            if(!$value$plusargs("SD_DISK_BYTES=%d",disk_bytes)||disk_bytes<=0||disk_bytes>2097152)
                $fatal(1,"invalid SD_DISK_BYTES");
            for(disk_index=0;disk_index<2097152;disk_index=disk_index+1)disk_image[disk_index]=8'h00;
            $readmemh(disk_file,disk_image,0,disk_bytes-1);
        end
    end
    initial begin
        image_bytes=0;
        have_image=$value$plusargs("SD_IMAGE=%s",image_file);
        if(have_image!=0) begin
            if(!$value$plusargs("SD_IMAGE_BYTES=%d",image_bytes) || image_bytes<=0 || image_bytes>65536)
                $fatal(1,"invalid SD_IMAGE_BYTES");
            for(image_index=0;image_index<65536;image_index=image_index+1)card_image[image_index]=8'hff;
            $readmemh(image_file,card_image,0,image_bytes-1);
        end
    end

    function automatic [7:0] crc7;
        integer k,b;
        reg [6:0] crc;
        reg feedback;
        begin
            crc=0;
            for(k=0;k<5;k=k+1) for(b=7;b>=0;b=b-1) begin
                feedback=crc[6]^command[k][b]; crc={crc[5:0],1'b0};
                if(feedback) crc=crc^7'h09;
            end
            crc7={crc,1'b1};
        end
    endfunction
    function automatic [15:0] crc16_byte(input [15:0] old_crc,input [7:0] value);
        integer b;
        reg [15:0] crc;
        begin
            crc=old_crc^{value,8'h00};
            for(b=0;b<8;b=b+1) crc=crc[15]?((crc<<1)^16'h1021):(crc<<1);
            crc16_byte=crc;
        end
    endfunction
    function automatic [7:0] sector_value(input [31:0] lba,input integer index);
        reg [31:0] value;
        reg [63:0] image_offset;
        begin
            value=lba*32'd37+32'(index)*32'd13+(32'(index)>>3);
            image_offset=(({32'd0,lba}-64'd16)<<9)+64'(index);
            if(have_disk!=0)begin
                image_offset=({32'd0,lba}<<9)+64'(index);
                if(image_offset<64'(disk_bytes))sector_value=disk_image[image_offset[20:0]];
                else sector_value=8'h00;
            end else if(have_image!=0 && lba>=16 && image_offset<64'(image_bytes))
                sector_value=card_image[image_offset[15:0]];
            else sector_value=value[7:0];
        end
    endfunction
    task automatic enqueue(input [7:0] value);
        begin
            response[response_tail]=value;
            response_tail=(response_tail+1)&1023;
        end
    endtask
    task automatic enqueue_sector(input [31:0] lba);
        integer n;
        reg [15:0] crc;
        reg [7:0] value;
        reg inject_this_read;
        begin
            inject_this_read=(FAULT_ONCE==0 || read_count==1);
            stream_stalled=(STALL_READ!=0 && inject_this_read);
            if(!(STALL_READ!=0 && inject_this_read)) begin
                enqueue(8'hff); enqueue(8'hfe); crc=0;
                for(n=0;n<512;n=n+1) begin
                    value=sector_value(lba,n); enqueue(value); crc=crc16_byte(crc,value);
                end
                if(CORRUPT_CRC!=0 && inject_this_read) crc=crc^16'h0001;
                enqueue(crc[15:8]); enqueue(crc[7:0]);
            end
        end
    endtask
    task automatic load_output;
        begin
            // Generate only a real next card sector when the previous CRC
            // has left the serial queue; the host never supplies payload.
            if(response_head==response_tail && multiblock!=0 &&
               stream_stalled==0 && command_length==0) begin
                read_count=read_count+1; enqueue_sector(next_lba); next_lba=next_lba+1;
            end
            if(response_head!=response_tail) begin
                out_shift=response[response_head]; response_head=(response_head+1)&1023;
            end else if(stop_busy_remaining!=0) begin
                out_shift=8'h00;
                if(stop_busy_remaining>0) stop_busy_remaining=stop_busy_remaining-1;
            end else out_shift=8'hff;
            out_bit=7; spi_miso=out_shift[7];
        end
    endtask
    task automatic process_command;
        reg [31:0] argument;
        reg [7:0] idle_r1;
        begin
            argument={command[1],command[2],command[3],command[4]};
            idle_r1=card_idle?8'h01:8'h00;
            if(powerup_clocks<REQUIRE_POWERUP_CLOCKS) begin
                // An incorrect CS-low wakeup sequence must not pass this model.
            end else if(crc7()!=command[5]) enqueue(idle_r1|8'h08);
            else begin
                case(command[0]&8'h3f)
                    0: begin
                        response_head=0; response_tail=0; multiblock=0; stream_stalled=0; stop_busy_remaining=0;
                        enqueue(8'h01); card_idle=1; app_command=0; acmd41_count=0;
                    end
                    8: begin
                        if(SDHC!=0) begin
                            enqueue(idle_r1); enqueue(8'h00); enqueue(8'h00);
                            enqueue(8'h01); enqueue(8'haa);
                        end else enqueue(idle_r1|8'h04);
                    end
                    55: begin enqueue(idle_r1); app_command=1; end
                    41: begin
                        if(app_command) begin
                            acmd41_count=acmd41_count+1;
                            if(acmd41_count>=READY_AFTER_ACMD41) card_idle=0;
                            enqueue(card_idle?8'h01:8'h00); app_command=0;
                        end else enqueue(idle_r1|8'h04);
                    end
                    1: begin card_idle=0; enqueue(8'h00); end
                    58: begin
                        enqueue(idle_r1); enqueue(SDHC!=0?8'hc0:8'h80);
                        enqueue(8'hff); enqueue(8'h80); enqueue(8'h00);
                    end
                    16: enqueue(argument==512?idle_r1:(idle_r1|8'h40));
                    12: begin
                        // SPI CMD12 interrupts a queued sector and has one
                        // stuff byte, then R1b and a bounded/configured busy.
                        response_head=0; response_tail=0; multiblock=0; stream_stalled=0;
                        enqueue(8'h7f); enqueue(idle_r1); stop_busy_remaining=stop_busy_bytes;
                    end
                    17,18: begin
                        if((command[0]&8'h3f)==18 && REJECT_CMD18!=0) begin
                            enqueue(8'h04); stop_busy_remaining=stop_busy_bytes;
                        end
                        else if(card_idle) enqueue(8'h01);
                        else if(SDHC==0 && argument[8:0]!=0) enqueue(8'h40);
                        else begin
                            enqueue(8'h00); stream_stalled=0;
                            if((command[0]&8'h3f)==18) begin
                                multiblock=1; next_lba=SDHC!=0?argument:(argument>>9);
                            end else begin
                                read_count=read_count+1;
                                enqueue_sector(SDHC!=0?argument:(argument>>9));
                            end
                        end
                    end
                    default: enqueue(idle_r1|8'h04);
                endcase
            end
        end
    endtask
    task automatic reset_state;
        begin
            response_head=0; response_tail=0; in_bit=0; out_bit=7;
            command_length=0; in_shift=0; out_shift=8'hff;
            app_command=0; card_idle=1; acmd41_count=0; read_count=0;
            powerup_clocks=0; spi_miso=1;
            multiblock=0; next_lba=0; stream_stalled=0; stop_busy_remaining=0;
        end
    endtask
    initial reset_state();
    always @(posedge reset) reset_state();
    always @(negedge spi_cs_n) begin
        command_length=0; in_bit=0; in_shift=0; out_shift=8'hff; out_bit=7; spi_miso=1;
    end
    always @(posedge spi_cs_n) begin
        in_bit=0; command_length=0; response_head=0; response_tail=0; spi_miso=1;
    end
    always @(posedge spi_clk) begin
        if(!reset) begin
            if(spi_cs_n) begin
                if(spi_mosi && powerup_clocks<100000) powerup_clocks=powerup_clocks+1;
            end else begin
                in_shift={in_shift[6:0],spi_mosi};
                if(in_bit==7) begin
                    if(command_length<6) begin
                        if(command_length!=0 || in_shift[7:6]==2'b01) begin
                            command[command_length]=in_shift; command_length=command_length+1;
                            if(command_length==6) begin process_command(); command_length=0; end
                        end
                    end
                    in_bit=0;
                end else in_bit=in_bit+1;
            end
        end
    end
    always @(negedge spi_clk) begin
        if(!spi_cs_n && !reset) begin
            if(in_bit==0) load_output();
            else begin out_bit=out_bit-1; spi_miso=out_shift[out_bit]; end
        end else spi_miso=1;
    end
endmodule
