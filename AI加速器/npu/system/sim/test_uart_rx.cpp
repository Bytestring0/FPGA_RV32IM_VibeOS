#include "Vag_uart_rx_top.h"
#include "verilated.h"
#include <cstdio>
#include <stdexcept>
struct Bench{Vag_uart_rx_top d;unsigned clocks=0;
 void step(){d.clk=0;d.eval();d.clk=1;d.eval();clocks++;}
 void run(unsigned n){while(n--)step();}
 Bench(){d.rx_i=1;d.req_valid=0;d.rsp_ready=1;d.rst=1;run(4);d.rst=0;run(20);}
 void bit(unsigned value,unsigned n=64){d.rx_i=value;run(n);}
 void byte(unsigned v,unsigned duration=64,bool stop=true){bit(0,duration);for(unsigned i=0;i<8;i++)bit((v>>i)&1,duration);bit(stop,duration);if(!stop)bit(1,duration);}
 unsigned io(unsigned off,bool write=false,unsigned value=0,bool err=false){d.req_addr=0x10001240+off;d.req_valid=1;d.req_write=write;d.req_wdata=value;d.req_wstrb=15;while(!d.req_ready)step();step();d.req_valid=0;if(!d.rsp_valid||bool(d.rsp_error)!=err)throw std::runtime_error("MMIO");unsigned x=d.rsp_rdata;step();return x;}
};
static void need(bool x,const char*s){if(!x)throw std::runtime_error(s);}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);try{Bench b;need(b.io(8)==0x55525831,"id");b.io(0,false,0,true);
 b.bit(0,3);b.bit(1,25);need(!(b.io(4)&1),"short start noise");
 for(unsigned v: {0u,255u,0x55u,0xaau,0x81u})b.byte(v);b.run(10);need((b.io(4)>>8)==5,"fifo count");for(unsigned v: {0u,255u,0x55u,0xaau,0x81u})need(b.io(0)==v,"bit order");
 b.byte(0x39,63);b.byte(0xc6,65);b.run(12);need(b.io(0)==0x39&&b.io(0)==0xc6,"baud tolerance");
 for(unsigned i=0;i<33;i++)b.byte(i);b.run(10);need(((b.io(4)>>8)&63)==32&&(b.io(4)&2),"overflow status");for(unsigned i=0;i<32;i++)need(b.io(0)==i,"FIFO overwritten");b.io(4,true,2);need(!(b.io(4)&2),"clear overflow");
 b.byte(0,64,false);b.run(10);need((b.io(4)&4)&&!(b.io(4)&1),"bad stop");b.io(4,true,4);need(!(b.io(4)&4),"clear framing");
 b.byte(0x42);b.run(10);b.d.rsp_ready=0;b.d.req_addr=0x10001240;b.d.req_write=0;b.d.req_valid=1;while(!b.d.req_ready)b.step();b.step();b.d.req_valid=0;for(unsigned n=0;n<40;n++){b.step();need(b.d.rsp_valid&&b.d.rsp_rdata==0x42&&!b.d.req_ready,"held response");}b.d.rsp_ready=1;b.step();need(!(b.io(4)&1),"duplicate FIFO pop");
 std::printf("UART_RX_PASS clocks=%u fifo32 baud_tolerance noise framing overflow response_hold\n",b.clocks);return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"UART_RX_FAIL %s\n",e.what());return 1;}}
