#include "Vqinglong_l1cache.h"
#include "verilated.h"
#include <array>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using Line=std::array<uint32_t,8>;
static void check(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(1);}}
struct Bench {
 Vqinglong_l1cache t; uint64_t cycle=0,reads=0,writes=0; bool pending=false,err=false,fail_next=false; unsigned delay=0;Line response{};
 std::unordered_map<uint32_t,uint32_t> mem;
 uint32_t word(uint32_t n){auto i=mem.find(n);return i==mem.end()?(n*0x9e3779b9u)^0xa55aa55a:i->second;}
 bool step(){
  check(++cycle<500000,"timeout");t.clk=0;t.mem_req_ready=!pending && cycle%5!=0;t.mem_rsp_valid=pending&&!delay;t.mem_rsp_error=err;
  for(int i=0;i<8;i++)t.mem_rsp_data[i]=response[i];t.eval();
  bool accepted=t.req_valid&&t.req_ready;
  if(t.mem_rsp_valid&&t.mem_rsp_ready)pending=false;
  if(t.mem_req_valid&&t.mem_req_ready){
   check(!(t.mem_req_addr&31),"native alignment");err=fail_next;fail_next=false;uint32_t a=t.mem_req_addr>>2;
   if(t.mem_req_write){writes++;check(t.mem_req_wstrb==0xffffffffu,"full line writeback");if(!err)for(int i=0;i<8;i++)mem[a+i]=t.mem_req_wdata[i];}else reads++;
   for(int i=0;i<8;i++)response[i]=word(a+i);pending=true;delay=1+cycle%9;
  }else if(delay)delay--;
  t.clk=1;t.eval();return accepted;
 }
 Bench(){t.clk=0;t.rst=1;t.maintenance_req=0;t.req_valid=0;t.rsp_ready=0;t.mem_req_ready=0;t.mem_rsp_valid=0;t.mem_rsp_error=0;for(int i=0;i<8;i++)t.mem_rsp_data[i]=0;for(int i=0;i<5;i++)step();t.rst=0;step();}
 Line op(uint32_t a,bool ins=false,bool wr=false,Line data={},uint32_t mask=0,bool expect_error=false){
  t.req_valid=1;t.req_addr=a;t.req_instruction=ins;t.req_write=wr;t.req_wstrb=mask;for(int i=0;i<8;i++)t.req_wdata[i]=data[i];
  while(!step()){}t.req_valid=0;while(!t.rsp_valid)step();Line out{};for(int i=0;i<8;i++)out[i]=t.rsp_data[i];bool e=t.rsp_error;
  for(int n=0;n<3;n++){step();check(t.rsp_valid && t.rsp_error==e,"response hold");for(int i=0;i<8;i++)check(t.rsp_data[i]==out[i],"stable payload");}
  check(e==expect_error,"response error");t.rsp_ready=1;step();t.rsp_ready=0;return out;
 }
 void flush(){t.maintenance_req=1;step();t.maintenance_req=0;check(t.maintenance_busy,"maintenance latched");while(t.maintenance_busy){check(!t.req_ready,"maintenance excludes new requests");step();}check(!t.maintenance_error,"flush error");step();}
};
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);Bench b;
 auto a=b.op(0);auto n=b.reads;b.op(0);check(b.reads==n,"D hit avoids DDR");
 auto i=b.op(0,true);n=b.reads;b.op(0,true);check(b.reads==n,"I hit avoids DDR");
 Line w{};w[0]=0x12345678;w[7]=0x88776655;b.op(0,false,true,w,0xf000000f);
 check(b.op(0)[0]==w[0]&&b.op(0)[7]==w[7],"write hit merge");check(b.op(0,true)[0]==i[0],"I remains stale until fence");
 b.flush();check(b.word(0)==w[0]&&b.word(7)==w[7],"dirty flush to memory");check(b.op(0,true)[0]==w[0],"instruction coherence after maintenance");
 b.flush();b.op(0,false,true,w,15);b.op(128);n=b.reads;b.op(0);b.op(128);check(b.reads==n,"two ways coexist");b.op(256);check(b.word(0)==w[0],"dirty LRU victim written");
 std::unordered_map<uint32_t,uint32_t> ref;uint32_t rng=1234567;
 for(int k=0;k<500;k++){rng=rng*1664525u+1013904223u;uint32_t addr=((rng>>6)%256)*4;uint32_t old=ref.count(addr)?ref[addr]:b.word(addr>>2);bool wr=rng&1;unsigned lane=(addr>>2)&7;Line data{};data[lane]=rng;uint32_t mask=(rng>>20)&15;
  auto out=b.op(addr&~31u,false,wr,data,mask<<(lane*4));if(wr){for(int j=0;j<4;j++)if(mask>>j&1)old=(old&~(255u<<(j*8)))|(rng&(255u<<(j*8)));ref[addr]=old;}check(out[lane]==old,"random masked load/store");}
 b.flush();for(auto x:ref)check(b.word(x.first>>2)==x.second,"random writeback persistence");
 b.fail_next=true;b.op(0x10000,false,false,{},0,true);n=b.reads;b.op(0x10000);check(b.reads==n+1,"failed refill not installed");
 b.flush();b.op(0,false,true,w,15);b.op(128);b.fail_next=true;b.op(256,false,false,{},0,true);check(b.op(0)[0]==w[0],"failed eviction retains dirty victim");b.flush();check(b.word(0)==w[0],"retry persists dirty line");
 n=b.reads;b.op(3,false,false,{},0,true);b.op(0,true,true,{},0,true);check(b.reads==n,"invalid native request rejected");
 b.op(0x3fffffe0);b.flush();
 b.flush();b.op(0,false,true,w,15);b.fail_next=true;b.t.maintenance_req=1;b.step();b.t.maintenance_req=0;
 while(b.t.maintenance_busy)b.step();check(b.t.maintenance_error,"failed maintenance latches fault");
 b.op(0,true,false,{},0,true);b.op(0,false,false,{},0,true);
 printf("Cache PASS: 2-way hits, byte masks, dirty LRU eviction, FENCE coherence, backpressure, refill/writeback errors, random traffic; reads=%llu writes=%llu cycles=%llu\n",(unsigned long long)b.reads,(unsigned long long)b.writes,(unsigned long long)b.cycle);
}
