#include "Vag_button_top.h"
#include "verilated.h"
#include <cstdio>
#include <stdexcept>
struct Bench {
 Vag_button_top d; unsigned cycles=0;
 void step(){d.clk=0;d.eval();d.clk=1;d.eval();cycles++;}
 void run(unsigned n){while(n--)step();}
 Bench(){d.buttons_n=31;d.rsp_ready=1;d.req_valid=0;d.rst=1;run(3);d.rst=0;run(3);}
 uint32_t read(unsigned address){d.req_addr=address;d.req_write=0;d.req_valid=1;while(!d.req_ready)step();step();d.req_valid=0;if(!d.rsp_valid||d.rsp_error)throw std::runtime_error("MMIO read");auto x=d.rsp_rdata;step();return x;}
};
static void need(bool x,const char*s){if(!x)throw std::runtime_error(s);}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);try{Bench b;
 need(b.read(0x10001218)==0x4b455935,"id");need(b.read(0x10001210)==0,"reset");
 b.d.buttons_n=30;b.run(2);b.d.buttons_n=31;b.run(8);need((b.read(0x10001210)&0x7fff)==0,"bounce event");
 b.d.buttons_n=30;b.run(10);auto e=b.read(0x10001210);need((e&31)==1&&((e>>15)&31)==1,"press and level");
 need((b.read(0x10001210)&0x7fff)==0,"read clear");b.run(30);e=b.read(0x10001210);need(((e>>10)&31)==1,"long press");
 b.run(30);need((b.read(0x10001210)&0x7fff)==0,"long press repeated");
 b.d.buttons_n=31;b.run(12);e=b.read(0x10001210);need(((e>>5)&31)==1&&((e>>15)&31)==0,"release");
 b.d.buttons_n=0;b.run(12);e=b.read(0x10001210);need((e&31)==31,"simultaneous keys");
 b.d.rsp_ready=0;b.d.req_addr=0x10001214;b.d.req_valid=1;while(!b.d.req_ready)b.step();b.step();b.d.req_valid=0;auto held=b.d.rsp_rdata;
 b.d.buttons_n=31;for(unsigned n=0;n<40;n++){b.step();need(b.d.rsp_valid&&b.d.rsp_rdata==held&&!b.d.req_ready,"response backpressure");}
 b.d.rsp_ready=1;b.step();e=b.read(0x10001210);need(((e>>5)&31)==31,"release event lost while response held");
 b.d.req_addr=0x10001210;b.d.req_write=1;b.d.req_wdata=0;b.d.req_wstrb=15;b.d.req_valid=1;b.step();need(b.d.rsp_valid&&b.d.rsp_error,"write accepted");b.d.req_valid=0;b.step();
 std::printf("BUTTONS_PASS cycles=%u debounce=4 long=20 keys=5 bounce events backpressure read_clear\n",b.cycles);return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"BUTTONS_FAIL %s\n",e.what());return 1;}}
