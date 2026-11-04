#include "Vag_text_video.h"
#include "verilated.h"
#include <cstdio>
#include <stdexcept>
extern "C" {
#include "../sw/ag_text_display.h"
}
struct Bench {
 Vag_text_video d;unsigned ticks=0;bool pixel_running=true;
 void step(){d.clk=0;d.pixel_clk=0;d.eval();d.clk=1;d.eval();if(pixel_running){d.pixel_clk=1;d.eval();}ticks++;}
 void run(unsigned n){while(n--)step();}
 Bench(){d.req_valid=0;d.rsp_ready=1;d.hd_scl_i=d.hd_sda_i=1;d.rst=1;d.pixel_rst=1;run(5);d.rst=0;d.pixel_rst=0;run(2500);}
 uint32_t access(unsigned offset,bool write=false,unsigned value=0,bool error=false){d.req_valid=1;d.req_addr=0x10001100+offset;d.req_write=write;d.req_wdata=value;d.req_wstrb=15;unsigned n=0;while(!d.req_ready){step();if(++n>5000)throw std::runtime_error("request wait");}step();d.req_valid=0;if(!d.rsp_valid||bool(d.rsp_error)!=error)throw std::runtime_error("MMIO response");auto x=d.rsp_rdata;step();return x;}
 void wait_visible(bool visible){for(unsigned n=0;n<500000;n++){auto x=access(4);if(!(x&4)&&bool(x&2)==visible)return;}throw std::runtime_error("frame command acknowledgement");}
};
static uint32_t driver_read(void*u,uint32_t a){return static_cast<Bench*>(u)->access(a-0x10001100);}
static void driver_write(void*u,uint32_t a,uint32_t v){static_cast<Bench*>(u)->access(a-0x10001100,true,v);}
static uint64_t driver_clock(void*u){auto*b=static_cast<Bench*>(u);b->step();return b->ticks;}
static void driver_yield(void*u){static_cast<Bench*>(u)->step();}
static int glyph(void*,uint32_t cp,unsigned char rows[16]){for(unsigned q=0;q<16;q++)rows[q]=cp==32?0:255;return 0;}
static void need(bool x,const char*s){if(!x)throw std::runtime_error(s);}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);try{Bench b;
 need(b.access(28)==0x54585432,"id");need((b.access(4)&15)==0,"clear finished hidden");
 b.access(12,true,0);for(unsigned n=0;n<16;n++)b.access(16,true,0);
 b.access(12,true,65*16);for(unsigned n=0;n<16;n++)b.access(16,true,255);
 b.access(20,true,0);b.access(0,true,65);need(b.access(20)==1,"cursor");
 b.access(8,true,1);b.wait_visible(true);b.run(8);
 unsigned active=0,hs=0,vs=0,white=0;
 for(unsigned n=0;n<858*525;n++){b.step();active+=b.d.pixel_de;hs+=!b.d.pixel_hs;vs+=!b.d.pixel_vs;white+=b.d.pixel_de&&b.d.pixel_rgb==0xffffff;}
 need(active==720*480,"active pixel inventory");need(hs==62*525&&vs==6*858,"sync inventory");need(white==128,"glyph alignment or cell geometry");
 b.access(0,true,66,true);b.access(12,true,0,true);
 b.access(8,true,0);b.wait_visible(false);b.access(20,true,2399);b.access(0,true,65);b.access(0,true,66);need(b.access(4)&32,"overflow");
 b.access(8,true,2);b.run(2500);need(b.access(20)==0&&!(b.access(4)&32),"clear command");
 b.pixel_running=false;b.access(8,true,1);b.run(500080);need((b.access(4)&20)==20,"stopped pixel clock must retain pending ownership and report timeout");b.access(0,true,65,true);
 b.pixel_running=true;b.wait_visible(true);b.access(8,true,0);b.wait_visible(false);
 b.access(32,true,4);need(b.d.hd_scl_low&&b.d.hd_sda_low&&b.d.hd_reset_n,"open drain outputs");b.access(32,true,7);need(!b.d.hd_scl_low&&!b.d.hd_sda_low,"released I2C outputs");
 ag_text_display display{};ag_display_io io{&b,driver_read,driver_write,driver_clock,driver_yield,1000000};
 need(!ag_display_init(&display,&io,glyph,nullptr),"driver init");
 need(!ag_display_clear(&display,500000),"driver clear");
 need(!ag_display_text(&display,0,0,15,"ABCDEFGHIJKLMNOP",16),"driver glyph slots");
 need(b.access(20)==16,"glyph slots ten and thirteen were treated as control bytes");
 const char utf8[]={char(0xe4),char(0xb8),char(0xad),char(0xe6),char(0x96),char(0x87)};
 need(!ag_display_text(&display,16,0,15,utf8,sizeof utf8),"UTF8 glyph cache");
 need(display.glyph_count==19&&b.access(20)==18,"UTF8 cell inventory");
 const char invalid[]={char(0xc0),char(0xaf)};need(ag_display_text(&display,0,1,15,invalid,2)==-6,"invalid UTF8 accepted");
 need(!ag_display_present(&display,500000),"driver present");
 need(ag_display_text(&display,0,0,15,"A",1)==-2,"display ownership");
 need(!ag_display_hide(&display,500000),"driver hide");
 std::printf("TEXT_VIDEO_PASS active=%u hs_low=%u vs_low=%u glyph_pixels=%u stopped_clock_timeout ownership\n",active,hs,vs,white);return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"TEXT_VIDEO_FAIL %s\n",e.what());return 1;}}
