#include "Vnpu_tensor_v2.h"
#include "verilated.h"
#include "../sw/npu_tensor_v2.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <fstream>
struct Bench {
 Vnpu_tensor_v2 d; std::vector<uint8_t> mem; unsigned ticks=0, reads=0,writes=0; bool pending=false; unsigned delay=0,addr=0; bool fault=false,inject_read=false,inject_write=false; uint32_t data[8]={};
 bool held=false,heldwrite=false;unsigned heldaddr=0,heldstrobe=0;uint32_t helddata[8]={};
 Bench():mem(2<<20,0xa5){d.rsp_ready=1;reset();}
 void reset(){held=false;d.rst=1;step();step();d.rst=0;pending=false;d.req_valid=0;step();}
 void step(){d.clk=0; d.mem_req_ready=!pending&&(ticks%5!=1);d.mem_rsp_valid=pending&&delay==0;d.mem_rsp_error=fault;for(int i=0;i<8;i++)d.mem_rsp_data[i]=data[i];d.eval();if(held&&!d.rst){if(!d.mem_req_valid||d.mem_req_addr!=heldaddr||bool(d.mem_req_write)!=heldwrite||d.mem_req_wstrb!=heldstrobe)throw std::runtime_error("DDR request changed under backpressure");for(int q=0;q<8;q++)if(d.mem_req_wdata[q]!=helddata[q])throw std::runtime_error("DDR payload changed under backpressure");}held=!d.rst&&d.mem_req_valid&&!d.mem_req_ready;if(held){heldaddr=d.mem_req_addr;heldwrite=d.mem_req_write;heldstrobe=d.mem_req_wstrb;for(int q=0;q<8;q++)helddata[q]=d.mem_req_wdata[q];}bool take=d.mem_req_valid&&d.mem_req_ready;bool rsp=d.mem_rsp_valid&&d.mem_rsp_ready; if(take){addr=d.mem_req_addr;if(addr+32>mem.size())throw std::runtime_error("DDR outside test memory");fault=d.mem_req_write?inject_write:inject_read;if(d.mem_req_write){writes++;if(!fault)for(int b=0;b<32;b++)if((d.mem_req_wstrb>>b)&1)mem[addr+b]=(d.mem_req_wdata[b/4]>>(8*(b%4)))&255;}else{reads++;memcpy(data,mem.data()+addr,32);}pending=true;delay=1;}else if(rsp)pending=false;else if(pending&&delay)delay--;d.clk=1;d.eval();ticks++;}
 uint32_t csr(unsigned off,bool wr=false,uint32_t val=0){d.req_addr=0x10006000+off;d.req_write=wr;d.req_wdata=val;d.req_wstrb=15;d.req_valid=1;unsigned limit=0;while(!d.req_ready){step();if(++limit>100)throw std::runtime_error("MMIO ready");}step();d.req_valid=0;while(!d.rsp_valid){step();if(++limit>200)throw std::runtime_error("MMIO response");}if(d.rsp_error)throw std::runtime_error("MMIO error");auto r=d.rsp_rdata;step();return r;}
 void start(const npu_tensor_job &j){csr(0,true,2);const auto*p=(const uint32_t*)&j;for(int i=0;i<16;i++)csr(32+4*i,true,p[i]);csr(0,true,1);}
 void finish(){unsigned start=ticks;while(d.busy){step();if(ticks-start>20000000)throw std::runtime_error("completion timeout");}if(pending)throw std::runtime_error("done before DDR ACK");}
};
static int activation(unsigned i,unsigned seed){return int((i*13+seed)%255)-127;}
static int weight(unsigned r,unsigned i,unsigned seed){return int((r*11+i*7+seed)%16)-8;}
static uint16_t scale(unsigned r,unsigned g,unsigned seed){static const uint16_t v[]={1,0x3ff,0x400,0x401,0x7bf,0x1800,0x2800,0x3400,0x3c00,0x3fff,0x4000,0x7bff};return v[(r*7+g*5+seed)%12];}
static int64_t rne(__int128 x,uint64_t den){bool neg=x<0;unsigned __int128 a=neg?-x:x,q=a/den,r=a%den;if(r>den-r||(r==den-r&&(q&1)))q++;return neg?-int64_t(q):int64_t(q);}
static int32_t narrow(int64_t x){return x>INT32_MAX?INT32_MAX:x<INT32_MIN?INT32_MIN:int32_t(x);}
static void put(std::vector<uint8_t>&m,uint32_t a,uint32_t x){memcpy(m.data()+a,&x,4);}
int main(int argc,char **argv){Verilated::commandArgs(argc,argv);try{
 Bench b;
 for(auto shape:{std::pair<unsigned,unsigned>{9,4096},{16,2048}}){
  unsigned n=shape.first,k=shape.second,seed=41+k,g=(k+127)/128,os=((g+3)/4)*32;
  uint32_t ab=(0x1000+n*g*64+0x101f)&~31u;
  uint32_t raw=(ab+((k+31)/32)*32+0x101f)&~31u;
  uint32_t scales=(raw+n*os+127)&~31u,out=0x100000;
  std::vector<int32_t>want;
  for(unsigned r=0;r<n;r++){
   int64_t acc=0;
   for(unsigned group=0;group<g;group++){
    int32_t partial=0;for(unsigned j=0;j<128 && group*128+j<k;j++)partial+=weight(r,group*128+j,seed)*activation(group*128+j,seed);
    put(b.mem,raw+r*os+group*8,r|(group<<16));put(b.mem,raw+r*os+group*8+4,uint32_t(partial));
    uint16_t bits=scale(r,group,seed);memcpy(b.mem.data()+scales+(r*g+group)*2,&bits,2);
    unsigned e=(bits>>10)&31,m=(bits&1023)|(e?1024:0);int sh=e?int(e)-11:-10;
    __int128 num=__int128(m)*0x7fffffff;uint64_t den=127;if(sh>=0)num<<=sh;else den<<=-sh;
    acc+=rne(__int128(partial)*narrow(rne(num,den)),16384);
   }
   want.push_back(narrow(acc));
  }
  npu_tensor_job job{};job.opcode=19;job.n=n;job.m=g;job.a=raw;job.b=scales;job.out=out;job.param0=0x7fffffff;job.stride_a=os;
  unsigned r0=b.reads,w0=b.writes;b.start(job);b.finish();
  if(!b.d.done||b.d.error)throw std::runtime_error("tensor benchmark status");
  for(unsigned r=0;r<n;r++){int32_t got;memcpy(&got,b.mem.data()+out+r*4,4);if(got!=want[r])throw std::runtime_error("tensor benchmark numeric mismatch");}
  std::printf("PERF_BASE_TENSOR n=%u k=%u cycles=%u reads=%u writes=%u exact=pass\n",n,k,unsigned(b.d.cycle_count),b.reads-r0,b.writes-w0);
 }
 return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL_BENCH %s\n",e.what());return 1;}}
