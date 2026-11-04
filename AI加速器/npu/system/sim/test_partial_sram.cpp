#include "Vnpu_partial_sram.h"
#include "verilated.h"
#include <array>
#include <deque>
#include <cstdio>
#include <stdexcept>
using Line=std::array<uint32_t,8>;
static void need(bool v,const char*s){if(!v)throw std::runtime_error(s);}
struct Bench{
 Vnpu_partial_sram d;unsigned tick=0,ddr_reads=0,ddr_writes=0;bool held=false;unsigned heldaddr=0;bool heldwrite=false;Line helddata{};unsigned heldmask=0,write_seed=0x12340000;
 struct Response{unsigned due;Line data;bool error;};std::deque<Response> returns;
 Bench(){d.rst=1;d.req_valid=0;d.bus_req_valid=0;d.rsp_ready=1;d.bus_rsp_ready=1;d.mem_req_ready=0;d.mem_rsp_valid=0;step();step();d.rst=0;for(int i=0;i<10;i++)step();}
 bool step(){d.clk=0;d.mem_req_ready=(tick%5)!=1;d.mem_rsp_valid=!returns.empty()&&returns.front().due<=tick;d.mem_rsp_error=d.mem_rsp_valid&&returns.front().error;
  for(int i=0;i<8;i++)d.mem_rsp_data[i]=d.mem_rsp_valid?returns.front().data[i]:0;d.eval();
  bool accepted=d.bus_req_valid&&d.bus_req_ready;
  if(held&&!d.rst){need(d.mem_req_valid&&d.mem_req_addr==heldaddr&&d.mem_req_write==heldwrite&&d.mem_req_wstrb==heldmask,"held DDR changed");for(int i=0;i<8;i++)need(d.mem_req_wdata[i]==helddata[i],"held DDR data");}
  held=!d.rst&&d.mem_req_valid&&!d.mem_req_ready;if(held){heldaddr=d.mem_req_addr;heldwrite=d.mem_req_write;heldmask=d.mem_req_wstrb;for(int i=0;i<8;i++)helddata[i]=d.mem_req_wdata[i];}
  if(d.mem_rsp_valid&&d.mem_rsp_ready)returns.pop_front();
  if(d.mem_req_valid&&d.mem_req_ready){need(returns.size()<4,"DDR credits");Line data{};for(unsigned i=0;i<8;i++)data[i]=d.mem_req_addr+i;returns.push_back({tick+7,data,d.mem_req_addr==0xdead000});if(d.mem_req_write)ddr_writes++;else ddr_reads++;}
  d.clk=1;d.eval();need(++tick<20000,"test deadline");return accepted;
 }
 unsigned csr(unsigned off,bool write=false,unsigned value=0,bool error=false){d.req_addr=0x10007000+off;d.req_write=write;d.req_wdata=value;d.req_wstrb=15;d.req_valid=1;while(!d.req_ready)step();step();d.req_valid=0;while(!d.rsp_valid)step();need(bool(d.rsp_error)==error,"CSR error");unsigned result=d.rsp_rdata;step();return result;}
 Line transfer(unsigned addr,bool write=false,unsigned mask=0xffffffff,bool error=false){d.bus_rsp_ready=0;d.bus_req_addr=addr;d.bus_req_write=write;d.bus_req_wstrb=mask;for(unsigned i=0;i<8;i++)d.bus_req_wdata[i]=write_seed+i;d.bus_req_valid=1;while(!step()){}d.bus_req_valid=0;d.clk=0;d.eval();while(!d.bus_rsp_valid)step();need(bool(d.bus_rsp_error)==error,"bus response error");Line result{};for(int i=0;i<8;i++)result[i]=d.bus_rsp_data[i];d.bus_rsp_ready=1;step();return result;}
 void run(){puts("STAGE identity");need(csr(8)==0x50535231&&csr(12)==256,"identity/capacity");csr(4,true,0x100);csr(0,true,1);while(csr(0)&2){};
  puts("STAGE local");need(transfer(0x100)[0]==0,"initialized zero refill");transfer(0x100,true,1);need(transfer(0x100)[0]==0,"partial mask preserves zero bytes");
  transfer(0x100,true);auto r=transfer(0x100);need(r[0]==0x12340000&&r[7]==0x12340007,"SRAM round trip");
  write_seed=0x5678abcd;transfer(0x100,true,0xf);r=transfer(0x100);need(r[0]==write_seed&&r[7]==0x12340007,"byte masks");write_seed=0x12340000;transfer(0x100,true);
  transfer(0x1e0,true);need(transfer(0x1e0)[7]==0x12340007,"last SRAM line");
  transfer(0x101,true,0xffffffff,true);r=transfer(0x100);need(r[0]==0x12340000,"rejected unaligned write mutated SRAM");
  csr(4,true,0x200,true);need(csr(4)==0x100,"enabled remap rejected");
  d.bus_rsp_ready=0;d.bus_req_addr=0x100;d.bus_req_write=0;d.bus_req_valid=1;while(!d.bus_req_ready)step();step();d.bus_req_valid=0;for(int n=0;n<7;n++){need(d.bus_rsp_valid&&!d.bus_req_ready&&d.bus_rsp_data[0]==0x12340000,"local response hold");step();}csr(0,true,0,true);d.bus_rsp_ready=1;step();
  need(!ddr_reads&&!ddr_writes,"local transactions leaked to DDR");
  puts("STAGE ddr");transfer(0x80);transfer(0x200);transfer(0xdead000,false,0xffffffff,true);
  // Four DDR transactions precede a local read. Check ordered drain and local stall.
  puts("STAGE ordered");d.bus_rsp_ready=0;for(unsigned j=0;j<4;j++){d.bus_req_valid=1;d.bus_req_addr=0x800+j*32;d.bus_req_write=0;while(!step()){} }
  d.bus_req_valid=0;csr(0,true,0,true);csr(4,true,0x200,true);need(csr(4)==0x100,"inflight remap changed mapping");
  d.bus_req_valid=1;d.bus_req_addr=0x100;d.clk=0;d.eval();need(!d.bus_req_ready,"local overtook DDR");d.bus_req_valid=0;for(unsigned j=0;j<4;j++){d.bus_rsp_ready=0;step();while(!d.bus_rsp_valid)step();need(d.bus_rsp_data[0]==0x800+j*32,"DDR order");d.bus_rsp_ready=1;step();}r=transfer(0x100);need(r[0]==0x12340000,"SRAM after DDR drain");
  csr(0,true,0);csr(4,true,0x200);csr(0,true,1);while(csr(0)&2){};need(transfer(0x200)[0]==0,"reenable clears prior contents");transfer(0x200,true);r=transfer(0x200);need(r[0]==0x12340000,"new mapping");
  d.rst=1;step();d.rst=0;for(int n=0;n<10;n++)step();need(csr(0)==0,"reset disables mapping");transfer(0x200);
  printf("PARTIAL_SRAM_PASS cycles=%u ddr_reads=%u ddr_writes=%u ordered4=true masks=true invalid=true hold=true reset=true\n",tick,ddr_reads,ddr_writes);
 }
};
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);try{Bench b;b.run();return 0;}catch(const std::exception&e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
