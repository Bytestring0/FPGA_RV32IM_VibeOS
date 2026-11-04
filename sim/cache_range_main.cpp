#include "Vag_l1cache.h"
#include "verilated.h"
#include <array>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using Line=std::array<uint32_t,8>;
static void check(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(1);}}
struct Bench {
 Vag_l1cache t;uint64_t cycle=0,reads=0,writes=0;bool pending=false,err=false,fail_next=false,hold=false;unsigned delay=0;Line response{};
 std::unordered_map<uint32_t,uint32_t> mem;
 uint32_t word(uint32_t n){auto i=mem.find(n);return i==mem.end()?(n*0x9e3779b9u)^0xa55aa55a:i->second;}
 bool step(){
  check(++cycle<500000,"deadline");t.clk=0;t.mem_req_ready=!pending&&cycle%5!=0;t.mem_rsp_valid=pending&&!delay&&!hold;t.mem_rsp_error=err;
  for(int i=0;i<8;i++)t.mem_rsp_data[i]=response[i];t.eval();bool accepted=t.req_valid&&t.req_ready;
  if(t.mem_rsp_valid&&t.mem_rsp_ready)pending=false;
  if(t.mem_req_valid&&t.mem_req_ready){
   check(!(t.mem_req_addr&31),"alignment");err=fail_next;fail_next=false;uint32_t a=t.mem_req_addr>>2;
   if(t.mem_req_write){writes++;check(t.mem_req_wstrb==0xffffffffu,"full line");if(!err)for(int i=0;i<8;i++)mem[a+i]=t.mem_req_wdata[i];}else reads++;
   for(int i=0;i<8;i++)response[i]=word(a+i);pending=true;delay=1+cycle%9;
  }else if(delay)delay--;
  t.clk=1;t.eval();return accepted;
 }
 Bench(){t.rst=1;t.maintenance_req=0;t.maintenance_range=0;t.maintenance_start=0;t.maintenance_end=0;t.req_valid=0;t.rsp_ready=0;t.mem_rsp_valid=0;for(int i=0;i<5;i++)step();t.rst=0;while(t.maintenance_busy)step();step();}
 Line op(uint32_t a,bool ins=false,bool wr=false,Line data={},uint32_t mask=0,bool expect_error=false){
  t.req_valid=1;t.req_addr=a;t.req_instruction=ins;t.req_write=wr;t.req_wstrb=mask;for(int i=0;i<8;i++)t.req_wdata[i]=data[i];
  while(!step()){}t.req_valid=0;while(!t.rsp_valid)step();Line out{};for(int i=0;i<8;i++)out[i]=t.rsp_data[i];bool e=t.rsp_error;
  for(int n=0;n<3;n++){step();check(t.rsp_valid&&t.rsp_error==e,"response hold");for(int i=0;i<8;i++)check(t.rsp_data[i]==out[i],"stable payload");}
  check(e==expect_error,"response error");t.rsp_ready=1;step();t.rsp_ready=0;return out;
 }
 void start(uint32_t lo,uint32_t hi,bool range=true){t.maintenance_range=range;t.maintenance_start=lo;t.maintenance_end=hi;t.maintenance_req=1;step();t.maintenance_req=0;check(t.maintenance_busy,"maintenance captured");}
 void wait(){while(t.maintenance_busy){check(!t.req_ready,"maintenance excludes requests");step();}step();}
 void flush(uint32_t lo=0,uint32_t hi=0,bool range=false){start(lo,hi,range);wait();check(!t.maintenance_error,"maintenance successful");}
};
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);
 {Bench b;Line w{},neighbor{};w[0]=0x12345678;w[7]=0x88776655;neighbor[0]=0xaabbccdd;
  auto oldI=b.op(0,true);auto oldN=b.op(128,true);b.op(0,false,true,w,0xf000000f);b.op(128,false,true,neighbor,15);
  uint32_t neighbor_mem=b.word(32);auto write_count=b.writes;
  b.start(0,32);b.t.maintenance_start=128;b.t.maintenance_end=160;b.wait();
  check(b.writes==write_count+1&&b.word(0)==w[0]&&b.word(7)==w[7],"range writes all selected line bytes");
  check(b.word(32)==neighbor_mem,"same-set other-way dirty remains private");
  auto reads=b.reads;check(b.op(128)[0]==neighbor[0]&&b.reads==reads,"unselected D line stays hit/dirty");
  reads=b.reads;check(b.op(128,true)[0]==oldN[0]&&b.reads==reads,"unselected I line preserved");
  check(b.op(0,true)[0]==w[0]&&oldI[0]!=w[0],"selected I line invalidated with D same physical line");
  b.flush(128,160,true);check(b.word(32)==neighbor[0],"later range writes retained neighbor");
  b.op(0,false,true,w,15);b.op(128,false,true,neighbor,15);write_count=b.writes;b.flush();check(b.writes==write_count+2,"legacy full flush retained");
  b.op(0x3fffffe0,false,true,w,15);b.flush(0x3fffffe0,0x40000000,true);check(b.word(0x3fffffe0>>2)==w[0],"exclusive upper DDR boundary");
 }
 {Bench b;Line w{};w[0]=0xabcdef01;b.op(0,false,true,w,15);b.hold=true;b.start(0,32);
  while(!b.pending)b.step();for(unsigned n=0;n<100;n++)b.step();check(b.t.maintenance_busy&&!b.t.req_ready,"deadline cannot cancel active writeback");
  b.hold=false;b.wait();check(!b.t.maintenance_error&&b.word(0)==w[0],"late ACK finishes owned maintenance");
 }
 {Bench b;Line w{};w[0]=0xface1234;b.op(0,false,true,w,15);b.fail_next=true;b.start(0,32);b.wait();
  check(b.t.maintenance_error,"range writeback failure sticky");check(b.word(0)!=w[0],"failed writeback not committed");
  b.op(0,false,false,{},0,true);b.op(0,true,false,{},0,true);
  // A later scan can demonstrate the dirty payload was retained; sticky error
  // deliberately remains and no software may publish success without reset.
  b.start(0,32);b.wait();check(b.word(0)==w[0]&&b.t.maintenance_error,"failed dirty victim retained and error not cleared");
 }
 {Bench b;Line w{};w[0]=0x654321ab;
  // A short range crosses the last set into set0; both tags and both caches matter.
  b.op(96,false,true,w,15);b.op(128,false,true,w,15);b.op(160,false,true,w,15);
  auto untouched=b.word(40);b.flush(96,160,true);
  check(b.word(24)==w[0]&&b.word(32)==w[0],"wrapped range writes both target sets");
  check(b.word(40)==untouched&&b.op(160)[0]==w[0],"wrapped range preserves following dirty set");
  b.flush(128,128,true);check(b.word(40)==untouched,"empty range preserves dirty data");
  b.flush(0,4096,true);check(b.word(40)==w[0],"large range falls back to complete scan");
  auto start=b.cycle;b.flush(0,32,true);
  std::printf("CACHE_RANGE_EMPTY_LINE_CYCLES=%llu\n",(unsigned long long)(b.cycle-start));
 }
 std::puts("PASS_CACHE_RANGE boundaries same-set-neighbor I/D-shared-line snapshot full-fallback late-ACK dirty-error-retention");return 0;
}
