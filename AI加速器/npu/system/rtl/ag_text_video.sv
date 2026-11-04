// 720x480p60 @27MHz external RGB transmitter; 80x30 cells with 40px margins.
// CPU writes while hidden. Display command completes at vertical blank; the
// toggle ACK returns ownership before firmware may modify the shared buffers.
module ag_text_video #(
 parameter [31:0] CSR_BASE=32'h10001100,
 parameter integer WAIT_CYCLES=102000000
)(
 input wire clk,rst,pixel_clk,pixel_rst,
 input wire req_valid,output wire req_ready,input wire req_write,
 input wire [31:0] req_addr,req_wdata,input wire [3:0] req_wstrb,
 output reg rsp_valid,input wire rsp_ready,output reg [31:0] rsp_rdata,output reg rsp_error,
 output reg [23:0] pixel_rgb,output reg pixel_hs,pixel_vs,pixel_de,
 input wire hd_scl_i,hd_sda_i,output reg hd_scl_low,hd_sda_low,hd_reset_n
);
 reg [15:0] cells[0:4095];reg [7:0] glyphs[0:4095];
 reg [11:0] cursor,glyph_address,clear_address;reg [7:0] attributes;
 reg clearing,desired_visible,command_toggle,waiting,command_timeout,overflow,command_error;
 reg [31:0] wait_count;
 (* async_reg="true" *) reg [1:0] ack_sync,visible_meta,command_sync;
 (* async_reg="true" *) reg [1:0] scl_sync,sda_sync;
 reg pixel_ack,pixel_visible;reg last_command;
 wire accepted=req_valid&&req_ready;
 wire data_write=accepted&&req_write&&req_addr==CSR_BASE;
 wire glyph_write=accepted&&req_write&&req_addr==CSR_BASE+16;
 assign req_ready=!rst&&!rsp_valid&&!clearing;
 always @(posedge clk)begin
  if(!rst)begin
   if(clearing)cells[clear_address]<=16'h0f00;
   else if(data_write&&!desired_visible&&!waiting&&req_wstrb[0]&&cursor<2400)
    cells[cursor]<={attributes,req_wdata[7:0]};
   if(glyph_write&&!desired_visible&&!waiting&&req_wstrb[0])glyphs[glyph_address]<=req_wdata[7:0];
  end
 end
 always @(posedge clk)begin
  if(rst)begin
   cursor<=0;glyph_address<=0;attributes<=8'h0f;clear_address<=0;clearing<=1;
   desired_visible<=0;command_toggle<=0;waiting<=0;wait_count<=0;command_timeout<=0;overflow<=0;command_error<=0;
   ack_sync<=0;visible_meta<=0;scl_sync<=3;sda_sync<=3;
   hd_scl_low<=0;hd_sda_low<=0;hd_reset_n<=0;rsp_valid<=0;rsp_rdata<=0;rsp_error<=0;
  end else begin
   ack_sync<={ack_sync[0],pixel_ack};visible_meta<={visible_meta[0],pixel_visible};
   scl_sync<={scl_sync[0],hd_scl_i};sda_sync<={sda_sync[0],hd_sda_i};
   if(clearing)begin
    clear_address<=clear_address+12'd1;
    if(clear_address==2399)begin clearing<=0;clear_address<=0;end
   end
   if(waiting)begin
    if(ack_sync[1]==command_toggle)begin waiting<=0;wait_count<=0;end
    else if(wait_count>=WAIT_CYCLES-1)begin command_timeout<=1;wait_count<=wait_count;end
    else wait_count<=wait_count+32'd1;
   end
   if(rsp_valid&&rsp_ready)rsp_valid<=0;
   if(accepted)begin
    rsp_valid<=1;rsp_rdata<=0;rsp_error<=req_addr[1:0]!=0;
    if(req_write)begin
     case(req_addr)
      CSR_BASE:begin
       if(desired_visible||waiting||!req_wstrb[0])rsp_error<=1;
       else if(cursor<2400)cursor<=cursor+12'd1;
       else overflow<=1;
      end
      CSR_BASE+8:begin
       if(waiting||!req_wstrb[0]||req_wdata>2)begin rsp_error<=1;command_error<=1;end
       else if(req_wdata==2)begin
        if(desired_visible)begin rsp_error<=1;command_error<=1;end
        else begin clearing<=1;cursor<=0;clear_address<=0;overflow<=0;end
       end else begin desired_visible<=req_wdata[0];command_toggle<=!command_toggle;waiting<=1;wait_count<=0;command_timeout<=0;end
      end
      CSR_BASE+12:if(req_wdata>=4096||desired_visible||waiting)rsp_error<=1;else glyph_address<=req_wdata[11:0];
      CSR_BASE+16:if(desired_visible||waiting||!req_wstrb[0])rsp_error<=1;else glyph_address<=glyph_address+12'd1;
      CSR_BASE+20:if(req_wdata>=2400||desired_visible||waiting)rsp_error<=1;else cursor<=req_wdata[11:0];
      CSR_BASE+24:if(!req_wstrb[0])rsp_error<=1;else attributes<=req_wdata[7:0];
      CSR_BASE+32:if(!req_wstrb[0])rsp_error<=1;else begin hd_scl_low<=!req_wdata[0];hd_sda_low<=!req_wdata[1];hd_reset_n<=req_wdata[2];end
      default:rsp_error<=1;
     endcase
    end else begin
     case(req_addr)
      CSR_BASE+4:rsp_rdata<={10'd0,cursor,3'd0,command_error,overflow,command_timeout,clearing,waiting,visible_meta[1],desired_visible};
      CSR_BASE+12:rsp_rdata<={20'd0,glyph_address};
      CSR_BASE+20:rsp_rdata<={20'd0,cursor};
      CSR_BASE+24:rsp_rdata<={24'd0,attributes};
      CSR_BASE+28:rsp_rdata<=32'h54585432;
      CSR_BASE+32:rsp_rdata<={29'd0,hd_reset_n,sda_sync[1],scl_sync[1]};
      default:rsp_error<=1;
     endcase
    end
   end
  end
 end
 reg [9:0] hx,vy;
 wire hactive=hx<720,vactive=vy<480;
 wire text_area=hactive&&vactive&&hx>=40&&hx<680;
 wire [6:0] column=(hx-10'd40)>>3;
 wire [11:0] cell_address={3'd0,vy[8:4],4'd0}+{1'd0,vy[8:4],6'd0}+{5'd0,column};
 reg [15:0] cell_q;reg [7:0] glyph_q;
 reg [2:0] bit_q1,bit_q2;reg [3:0] row_q1;
 reg [7:0] attr_q2;reg [2:0] hs_pipe,vs_pipe,de_pipe,text_pipe;
 function automatic [23:0] color(input [3:0] index);
  begin color={index[2]?(index[3]?8'hff:8'hb0):8'h08,index[1]?(index[3]?8'hff:8'hb0):8'h0c,index[0]?(index[3]?8'hff:8'hb0):8'h18};end
 endfunction
 always @(posedge pixel_clk)begin
  cell_q<=cells[cell_address];row_q1<=vy[3:0];bit_q1<=3'd7-hx[2:0];
  glyph_q<=glyphs[{cell_q[7:0],row_q1}];attr_q2<=cell_q[15:8];bit_q2<=bit_q1;
  if(pixel_rst)begin hx<=0;vy<=0;command_sync<=0;last_command<=0;pixel_ack<=0;pixel_visible<=0;hs_pipe<=0;vs_pipe<=0;de_pipe<=0;text_pipe<=0;pixel_rgb<=0;pixel_hs<=1;pixel_vs<=1;pixel_de<=0;end
  else begin
   command_sync<={command_sync[0],command_toggle};
   if(hx==857)begin hx<=0;if(vy==524)vy<=0;else vy<=vy+10'd1;end else hx<=hx+10'd1;
   if(hx==0&&vy==480&&command_sync[1]!=last_command)begin
    pixel_visible<=desired_visible;last_command<=command_sync[1];pixel_ack<=command_sync[1];
   end
   hs_pipe<={hs_pipe[1:0],!(hx>=736&&hx<798)};vs_pipe<={vs_pipe[1:0],!(vy>=489&&vy<495)};
   de_pipe<={de_pipe[1:0],hactive&&vactive};text_pipe<={text_pipe[1:0],text_area&&pixel_visible};
   pixel_hs<=hs_pipe[1];pixel_vs<=vs_pipe[1];pixel_de<=de_pipe[1];
   pixel_rgb<=text_pipe[1]?color(glyph_q[bit_q2]?attr_q2[3:0]:attr_q2[7:4]):24'h080c18;
  end
 end
endmodule
