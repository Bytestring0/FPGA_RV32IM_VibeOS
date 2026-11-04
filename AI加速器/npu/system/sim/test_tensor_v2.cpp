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
#include <array>
#include <deque>
struct Bench {
 Vnpu_tensor_v2 d; std::vector<uint8_t> mem; unsigned ticks=0, reads=0,writes=0; bool pending=false;bool inject_read=false,inject_write=false;
 struct Response {unsigned due;std::array<uint32_t,8> data{};bool error;};std::deque<Response> responses;
 unsigned capacity=1,memory_latency=2,peak_pending=0;
 bool held=false,heldwrite=false;unsigned heldaddr=0,heldstrobe=0;uint32_t helddata[8]={};
 Bench():mem(2<<20,0xa5){if(const char*s=std::getenv("NPU_BENCH_DEPTH"))capacity=std::stoul(s);if(const char*s=std::getenv("NPU_BENCH_LATENCY"))memory_latency=std::stoul(s);d.rsp_ready=1;reset();}
 void reset(){held=false;responses.clear();d.rst=1;step();step();d.rst=0;pending=false;d.req_valid=0;step();}
 void step(){
  d.clk=0;d.mem_req_ready=!d.rst&&responses.size()<capacity&&(ticks%5!=1);
  d.mem_rsp_valid=!d.rst&&!responses.empty()&&responses.front().due<=ticks;
  d.mem_rsp_error=d.mem_rsp_valid&&responses.front().error;
  for(int q=0;q<8;q++)d.mem_rsp_data[q]=responses.empty()?0:responses.front().data[q];
  d.eval();
  if(held&&!d.rst){
   if(!d.mem_req_valid||d.mem_req_addr!=heldaddr||bool(d.mem_req_write)!=heldwrite||d.mem_req_wstrb!=heldstrobe)throw std::runtime_error("DDR request changed under backpressure");
   for(int q=0;q<8;q++)if(d.mem_req_wdata[q]!=helddata[q])throw std::runtime_error("DDR payload changed under backpressure");
  }
  held=!d.rst&&d.mem_req_valid&&!d.mem_req_ready;
  if(held){heldaddr=d.mem_req_addr;heldwrite=d.mem_req_write;heldstrobe=d.mem_req_wstrb;for(int q=0;q<8;q++)helddata[q]=d.mem_req_wdata[q];}
  bool take=d.mem_req_valid&&d.mem_req_ready,ack=d.mem_rsp_valid&&d.mem_rsp_ready;
  if(ack)responses.pop_front();
  if(take){
   unsigned addr=d.mem_req_addr;if(addr+32>mem.size())throw std::runtime_error("DDR outside test memory");
   Response response{};response.due=ticks+memory_latency+(ticks%3)+1;response.error=d.mem_req_write?inject_write:inject_read;
   if(d.mem_req_write){writes++;if(!response.error)for(int q=0;q<32;q++)if((d.mem_req_wstrb>>q)&1)mem[addr+q]=(d.mem_req_wdata[q/4]>>(8*(q%4)))&255;}
   else {reads++;memcpy(response.data.data(),mem.data()+addr,32);}
   responses.push_back(response);peak_pending=std::max(peak_pending,unsigned(responses.size()));
  }
  if(d.rst)responses.clear();pending=!responses.empty();d.clk=1;d.eval();ticks++;
 }
 uint32_t csr(unsigned off,bool wr=false,uint32_t val=0){d.req_addr=0x10006000+off;d.req_write=wr;d.req_wdata=val;d.req_wstrb=15;d.req_valid=1;unsigned limit=0;while(!d.req_ready){step();if(++limit>100)throw std::runtime_error("MMIO ready");}step();d.req_valid=0;while(!d.rsp_valid){step();if(++limit>200)throw std::runtime_error("MMIO response");}if(d.rsp_error)throw std::runtime_error("MMIO error");auto r=d.rsp_rdata;step();return r;}
 void start(const npu_tensor_job &j){csr(0,true,2);const auto*p=(const uint32_t*)&j;for(int i=0;i<16;i++)csr(32+4*i,true,p[i]);csr(0,true,1);}
 void finish(){unsigned start=ticks;while(d.busy){step();if(ticks-start>20000000)throw std::runtime_error("completion timeout");}if(pending)throw std::runtime_error("done before DDR ACK");}
};
static void put(std::vector<uint8_t>&m,unsigned a,int32_t x){memcpy(m.data()+a,&x,4);}
static unsigned tests=0;
static void fixtures(Bench &b,const char *path){
 std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("fixture input");
 auto word=[&](){uint32_t v=0;f.read((char*)&v,4);if(!f)throw std::runtime_error("truncated fixture");return v;};
 unsigned count=word();for(unsigned fixture=0;fixture<count;fixture++){
  npu_tensor_job job{};for(unsigned i=0;i<16;i++)((uint32_t*)&job)[i]=word();
  unsigned capacity=word();b.mem.resize(capacity);f.read((char*)b.mem.data(),capacity);
  std::vector<uint8_t> expected(capacity);f.read((char*)expected.data(),capacity);
  uint32_t error=word(),result=word(),numeric[6];for(auto &v:numeric)v=word();
  b.start(job);b.finish();
  if(b.d.error_code!=error||bool(b.d.error)!=bool(error)||(!error&&!b.d.done))throw std::runtime_error("fixture status "+std::to_string(fixture));
  if(expected!=b.mem){for(unsigned i=0;i<capacity;i++)if(expected[i]!=b.mem[i]){fprintf(stderr,"fixture %u op%u byte%x got%02x expect%02x\n",fixture,job.opcode,i,b.mem[i],expected[i]);break;}throw std::runtime_error("fixture DDR mismatch");}
  unsigned offsets[]={0x60,0x64,0x1c,0x68,0x70,0x6c};
  if(b.csr(0x18)!=result)throw std::runtime_error("fixture result "+std::to_string(fixture));
  for(unsigned i=0;i<6;i++){auto actual=b.csr(offsets[i]);if(actual!=numeric[i])throw std::runtime_error("fixture "+std::to_string(fixture)+" op"+std::to_string(job.opcode)+" diagnostic "+std::to_string(offsets[i])+" got"+std::to_string(actual)+" expect"+std::to_string(numeric[i]));}
  tests++;
 }
 printf("TENSOR_V2_FRACTION_FIXTURES_PASS cases=%u\n",count);
}
static void check(Bench &b,npu_tensor_job j,bool snapshot=false){auto expected=b.mem;npu_tensor_mapping mapping={expected.data(),expected.size(),0};npu_tensor_stats s{};auto rc=npu_tensor_reference(&j,&mapping,&s);unsigned oldwrites=b.writes;b.start(j);if(snapshot)b.csr(32+7*4,true,j.a);b.finish();if(b.d.error_code!=rc||bool(b.d.error)!=bool(rc)||(!rc&&!b.d.done))throw std::runtime_error("status opcode "+std::to_string(j.opcode)+" got "+std::to_string(b.d.error_code)+" expect "+std::to_string(rc));if(expected!=b.mem){for(unsigned i=0;i<expected.size();i++)if(expected[i]!=b.mem[i]){fprintf(stderr,"op %u mismatch byte %x got %02x expect %02x\n",j.opcode,i,b.mem[i],expected[i]);break;}throw std::runtime_error("numeric/memory mismatch");}if(rc&&b.writes!=oldwrites)throw std::runtime_error("invalid wrote DDR");if(!rc){if(b.csr(0x18)!=s.result)throw std::runtime_error("result CSR");if(b.csr(0x1c)!=s.numeric.saturations)throw std::runtime_error("sat count op "+std::to_string(j.opcode));if(b.csr(0x60)!=s.numeric.status)throw std::runtime_error("numeric status op "+std::to_string(j.opcode)+" got "+std::to_string(b.csr(0x60))+" expect "+std::to_string(s.numeric.status));if(b.csr(0x64)!=s.numeric.domain_clamps||b.csr(0x68)!=s.numeric.underflows||b.csr(0x6c)!=s.numeric.zero_vectors)throw std::runtime_error("numeric counters op "+std::to_string(j.opcode));}tests++;}

static void check_all_stats(Bench &b,const npu_tensor_job &j){
 auto expected=b.mem;npu_tensor_mapping map={expected.data(),expected.size(),0};npu_tensor_stats st{};
 auto rc=npu_tensor_reference(&j,&map,&st);unsigned prior=b.writes;
 b.start(j);b.finish();
 if(b.d.error_code!=rc||bool(b.d.error)!=bool(rc)||bool(b.d.done)==bool(rc)||b.mem!=expected)
  throw std::runtime_error("pipeline completion/memory mismatch op"+std::to_string(j.opcode));
 if(rc&&b.writes!=prior)throw std::runtime_error("pipeline invalid wrote DDR");
 uint32_t want[]={st.numeric.status,st.numeric.domain_clamps,st.numeric.saturations,st.numeric.underflows,st.numeric.invalid_inputs,st.numeric.zero_vectors};
 unsigned off[]={0x60,0x64,0x1c,0x68,0x70,0x6c};
 // Read immediately after !busy, without inserting an arbitrary drain wait.
 for(unsigned i=0;i<6;i++)if(b.csr(off[i])!=want[i])throw std::runtime_error("pipeline final stats mismatch op"+std::to_string(j.opcode)+" field"+std::to_string(i));
 tests++;
}
static void vector_prefetch_benchmark(Bench &b){
 b.mem.resize(2<<20,0xa5);
 for(unsigned n:{64u,65u,128u,4096u})for(unsigned op:{1u,2u,3u,5u,15u,16u,22u}){
  npu_tensor_job j{};j.opcode=op;j.n=n;j.a=0x1000;j.b=0x50000;j.out=0x100000;j.param0=4295;j.param1=1;
  for(unsigned q=0;q<n;q++){put(b.mem,j.a+4*q,int32_t((q*7919u)%200001u)-100000);put(b.mem,j.b+4*q,int32_t((q*6151u)%180001u)-90000);}
  unsigned start=b.ticks,reads=b.reads;check_all_stats(b,j);
  printf("VECTOR_PREFETCH_PASS op=%u n=%u reads=%u cycles=%u\n",op,n,b.reads-reads,b.ticks-start);
 }
 npu_tensor_job j{};j.opcode=3;j.n=128;j.a=0x1000;j.b=0x50000;j.out=0x100000;
 check_all_stats(b,j);put(b.mem,j.a,1234567);put(b.mem,j.b,-7654321);check_all_stats(b,j);
 auto before=b.mem;unsigned writes=b.writes;b.inject_read=true;b.start(j);b.finish();b.inject_read=false;
 if(!b.d.error||b.d.error_code!=10||b.d.done||b.mem!=before||b.writes!=writes)throw std::runtime_error("vector prefetch read error published output");
 check_all_stats(b,j);
 for(unsigned op:{2u,3u}){
  j.opcode=op;
  for(unsigned q=0;q<j.n;q++){put(b.mem,j.a+4*q,(q&1)?INT32_MIN:INT32_MAX);put(b.mem,j.b+4*q,(q&2)?INT32_MIN:INT32_MAX);}
  check_all_stats(b,j);
 }
 if(b.capacity>1&&b.memory_latency>=24&&b.peak_pending<8)throw std::runtime_error("prefetch did not exercise multiple outstanding requests");
 printf("VECTOR_PREFETCH_JOB_REFRESH_FAULT_PASS depth=%u latency=%u peak_pending=%u\n",b.capacity,b.memory_latency,b.peak_pending);
}
static void norm_benchmark(Bench &b){
 b.mem.resize(2<<20,0xa5);
 for(unsigned n:{1u,3u,64u,128u,1024u,4096u})for(unsigned op:{8u,22u}){
  npu_tensor_job j{};j.opcode=op;j.n=n;j.a=0x1000;j.b=0x50000;j.out=0x100000;j.param0=4295;j.param1=1;
  for(unsigned i=0;i<n;i++){put(b.mem,j.a+4*i,int(i*7919%200001)-100000);put(b.mem,j.b+4*i,int(i*17%10000)-5000);}
  unsigned start=b.ticks;check_all_stats(b,j);
  printf("NORM_BENCH_PASS op=%u n=%u ticks=%u engine_cycles=%u\n",op,n,b.ticks-start,b.d.cycle_count);
 }
}
static void norm_extremes(Bench &b){
 for(unsigned n:{64u,128u,1024u,4096u})for(unsigned op:{8u,22u})for(unsigned mode:{0u,1u,2u}){
  npu_tensor_job j{};j.opcode=op;j.n=n;j.a=0x1000;j.b=0x50000;j.out=0x100000;j.param0=mode?1u:4295u;j.param1=mode==2;
  for(unsigned q=0;q<n;q++){
   put(b.mem,j.a+q*4,mode==0?((q&1)?INT32_MIN:INT32_MAX):mode==1?0:q==n-1?INT32_MIN:1);
   put(b.mem,j.b+q*4,mode==2?((q&1)?INT32_MIN:INT32_MAX):65536);
  }
  check_all_stats(b,j);
 }
 puts("NORM_WIDE_SQUARE_EXTREMES_PASS");
}
static void delta_column_benchmark(Bench &b){
 b.mem.resize(2<<20,0xa5);
 for(unsigned op:{13u,20u})for(unsigned n:{8u,16u,128u}){
  npu_tensor_job j{};j.opcode=op;j.n=n;j.a=0x1000;j.b=0x50000;j.c=0xa0000;
  j.out=0x100000;j.state=0x110000;j.param0=1030792151;j.param1=536870912;
  for(unsigned i=0;i<n;i++){
   put(b.mem,j.a+4*i,int(i*7919%200001)-100000);
   put(b.mem,j.b+4*i,int(i*6151%180001)-90000);
   put(b.mem,j.c+4*i,int(i*3563%130001)-65000);
  }
  for(unsigned i=0;i<n*n;i++)put(b.mem,j.state+4*i,int(i*100003u%32000001u)-16000000);
  unsigned reads=b.reads,ticks=b.ticks;check_all_stats(b,j);
  printf("DELTA_COLUMN_BENCH_PASS op=%u n=%u reads=%u ticks=%u engine_cycles=%u\n",op,n,b.reads-reads,b.ticks-ticks,b.d.cycle_count);
  check_all_stats(b,j); // state read-modify-write must work on a second token.
  if(n==8||n==128){
   j.param0=j.param1=1073741824;
   for(unsigned i=0;i<n;i++){put(b.mem,j.a+4*i,i&1?INT32_MIN:INT32_MAX);put(b.mem,j.b+4*i,INT32_MAX);put(b.mem,j.c+4*i,INT32_MIN);}
   for(unsigned i=0;i<n*n;i++)put(b.mem,j.state+4*i,i&1?INT32_MIN:INT32_MAX);
   check_all_stats(b,j);
   // Three positive INT64 products overflow before later negative rows cancel.
   // Both II2 inner loops must retire in row order, including the last product.
   j.param1=0;
   for(unsigned i=0;i<n;i++){
    put(b.mem,j.a+4*i,i<3?INT32_MIN:INT32_MAX);
    put(b.mem,j.b+4*i,i<3?INT32_MIN:INT32_MAX);
   }
   for(unsigned i=0;i<n*n;i++)put(b.mem,j.state+4*i,INT32_MIN);
   check_all_stats(b,j);
  }
 }
 puts("DELTA_COLUMN_STATS_STATE_PASS");
}
static void conv_tap_benchmark(Bench &b){
 b.mem.resize(2<<20,0xa5);
 for(unsigned n:{1u,7u,8u,9u,128u,1024u})for(unsigned k:{1u,2u,4u,8u}){
  npu_tensor_job j{};j.opcode=12;j.n=n;j.k=k;j.a=0x1000;j.b=0x50000;j.out=0x100000;j.state=0x110000;
  for(unsigned i=0;i<n;i++)put(b.mem,j.a+4*i,int32_t((i*7919)%200001)-100000);
  for(unsigned i=0;i<n*k;i++)put(b.mem,j.b+4*i,int32_t((i*6151)%180001)-90000);
  for(unsigned i=0;i<n*(k-1);i++)put(b.mem,j.state+4*i,int32_t((i*100003u)%32000001u)-16000000);
  unsigned reads=b.reads;check_all_stats(b,j);
  printf("CONV_TAP_BENCH_PASS n=%u k=%u reads=%u engine_cycles=%u\n",n,k,b.reads-reads,b.d.cycle_count);
  check_all_stats(b,j);
 }
 // Deliberate INT64 overflow then cancellation checks the order of saturating
 // tap accumulation and its diagnostics; a balanced reduction is not equivalent.
 for(unsigned k:{4u,8u}){
  npu_tensor_job j{};j.opcode=12;j.n=8;j.k=k;j.a=0x1000;j.b=0x50000;j.out=0x100000;j.state=0x110000;
  for(unsigned i=0;i<j.n;i++)put(b.mem,j.a+4*i,INT32_MIN);
  for(unsigned i=0;i<j.n*k;i++)put(b.mem,j.b+4*i,i%k>=2?INT32_MAX:INT32_MIN);
  for(unsigned i=0;i<j.n*(k-1);i++)put(b.mem,j.state+4*i,INT32_MIN);
  check_all_stats(b,j);check_all_stats(b,j);
 }
 puts("CONV_TAP_STATS_STATE_PASS");
}
static void raw_stream_benchmark(Bench &b){
 b.mem.resize(2<<20,0xa5);npu_tensor_job j{};j.opcode=19;j.n=32;j.m=32;
 j.a=0x1000;j.b=0x50000;j.out=0x100000;j.stride_a=256;j.param0=65536;
 for(unsigned row=0;row<j.n;row++)for(unsigned group=0;group<j.m;group++){
  unsigned record=j.a+row*j.stride_a+group*8;
  put(b.mem,record,(group<<16)|row);put(b.mem,record+4,int(row*719+group*997)-16000);
  uint16_t scale=uint16_t(0x2400+((row*37+group*53)&0x7ff));
  memcpy(b.mem.data()+j.b+(row*j.m+group)*2,&scale,2);
 }
 unsigned reads=b.reads,ticks=b.ticks;check_all_stats(b,j);
 printf("RAW_STREAM_BENCH_PASS groups=%u reads=%u ticks=%u engine_cycles=%u\n",j.n*j.m,b.reads-reads,b.ticks-ticks,b.d.cycle_count);
 // A second submission mutates an already cached scale and must see the new DDR data.
 uint16_t changed=0x3000;memcpy(b.mem.data()+j.b,&changed,2);check_all_stats(b,j);
 puts("RAW_STREAM_JOB_INVALIDATION_PASS");
}
static void pipeline_boundaries(Bench &b){
 b.mem.resize(2<<20,0xa5);npu_tensor_job j{};j.a=0x1000;j.b=0x50000;j.c=0xa0000;j.out=0x100000;j.state=0x110000;
 unsigned tails[]={1,3,7,8,9,15,31,32,33,63};
 for(unsigned n:tails){
  j.opcode=15;j.n=n;
  for(unsigned i=0;i<n;i++)put(b.mem,j.a+4*i,int32_t(i*65537)-1000000);
  check_all_stats(b,j);
  j.opcode=1;check_all_stats(b,j);
 }
 // Exact quantization quotient/remainder boundaries, including INT32_MIN and ties.
 for(uint32_t magnitude:{1u,2u,3u,127u,128u,65535u,1073741824u,2147483647u,2147483648u}){
  j={};j.opcode=15;j.n=32;j.a=0x1000;j.out=0x100000;
  for(unsigned z=0;z<32;z++){
   int64_t value=z==0?int64_t(magnitude):int64_t((uint64_t(magnitude)*z)/31u);
   if(z&1)value=-value;
   if(value>INT32_MAX)value=INT32_MAX;
   put(b.mem,j.a+4*z,int32_t(value));
  }
  if(magnitude==2147483648u)put(b.mem,j.a,INT32_MIN);
  check_all_stats(b,j);
 }
 // Multiple clipping stages from a single stateful update must all count.
 j.b=0x50000;j.c=0xa0000;j.state=0x110000;j.opcode=20;j.n=1;j.param0=1073741824;j.param1=1073741824;
 put(b.mem,j.a,INT32_MAX);put(b.mem,j.b,INT32_MIN);put(b.mem,j.c,INT32_MAX);put(b.mem,j.state,INT32_MAX);
 check_all_stats(b,j);
 if(b.csr(0x1c)<2)throw std::runtime_error("pipeline multiple saturation stages not exercised");
 // BadScale clears preceding arithmetic diagnostics but preserves this edge's
 // INVALID_ARGUMENT exactly once. Neither partial output nor stale flags leak.
 j={};j.opcode=19;j.n=2;j.m=1;j.a=0x1000;j.b=0x50000;j.out=0x100000;j.stride_a=32;j.param0=0x80000000u;
 put(b.mem,j.a,0);put(b.mem,j.a+4,INT32_MAX);put(b.mem,j.a+32,1);put(b.mem,j.a+36,INT32_MAX);
 uint16_t scale[2]={0x7bff,0};memcpy(b.mem.data()+j.b,scale,sizeof scale);check_all_stats(b,j);
 if(!b.d.error||b.csr(0x70)!=1||b.csr(0x1c)||b.csr(0x68))throw std::runtime_error("pipeline BadScale clear ordering");
 b.csr(0,true,2);for(unsigned off:{0x60u,0x64u,0x1cu,0x68u,0x70u,0x6cu})if(b.csr(off))throw std::runtime_error("pipeline explicit clear");
 // Reset immediately after accepting the invalid-scale response must flush all
 // pending events. A following zero-vector job reports only its own zero flag.
 b.start(j);unsigned start=b.ticks;while(b.d.busy){b.step();if(b.ticks-start>100000)throw std::runtime_error("pipeline reset setup timeout");}
 b.reset();for(unsigned off:{0x60u,0x64u,0x1cu,0x68u,0x70u,0x6cu})if(b.csr(off))throw std::runtime_error("pipeline reset diagnostic leak");
 j={};j.opcode=15;j.n=1;j.a=0x1000;j.out=0x100000;put(b.mem,j.a,0);check_all_stats(b,j);
 puts("TENSOR_PACK_DIAG_BOUNDARIES_PASS byte_word_tails multi_sat badscale_clear reset_zero");
}

int main(int argc,char**argv){Verilated::commandArgs(argc,argv);try{Bench b;if(b.csr(0x10)!=NPU_TENSOR_ID||b.csr(0x14)!=NPU_TENSOR_NUMERIC_VERSION)throw std::runtime_error("identity");for(unsigned op=1;op<=18;op++){npu_tensor_job j={};j.opcode=op;j.n=op==10?10:9;j.m=3;j.k=op==12?4:7;j.a=0x1000;j.b=0x50000;j.c=0xa0000;j.out=0x100000;j.state=0x110000;if(op==8)j.param1=1;if(op==11)j.param0=1;if(op==13){j.param0=1030792151;j.param1=536870912;}if(op==14)j.param0=536870912;for(unsigned i=0;i<2000;i++){put(b.mem,j.a+4*i,(int32_t)((i*7919)%200001)-100000);put(b.mem,j.b+4*i,(int32_t)((i*6151)%180001)-90000);put(b.mem,j.c+4*i,(int32_t)((i*3563)%130001)-65000);put(b.mem,j.state+4*i,(int32_t)((i*100003)%32000001)-16000000);}if(op==10)for(unsigned i=0;i<j.n/2;i++){put(b.mem,j.b+4*i,900000000);put(b.mem,j.c+4*i,500000000);}check(b,j,true);if(op==12||op==13)check(b,j);auto bad=j;bad.out=bad.a;check(b,bad);bad=j;bad.a+=4;check(b,bad);bad=j;bad.out=0x40000000;bad.n=4096;check(b,bad);bad=j;bad.flags=1;check(b,bad);}
 npu_tensor_job j={};j.opcode=3;j.n=4;j.a=0x1000;j.b=0x50000;j.out=0x100000;int32_t a[]={-1,-3,INT32_MAX,INT32_MIN},c[]={32768,32768,INT32_MAX,INT32_MIN};for(int i=0;i<4;i++){put(b.mem,j.a+4*i,a[i]);put(b.mem,j.b+4*i,c[i]);}check(b,j);
 j.opcode=1;b.inject_read=true;auto before=b.mem;b.start(j);b.finish();if(!b.d.error||b.d.error_code!=10||b.mem!=before)throw std::runtime_error("read fault");b.inject_read=false;b.inject_write=true;b.start(j);b.finish();if(!b.d.error||b.d.error_code!=11||b.d.done)throw std::runtime_error("write fault");b.inject_write=false;b.start(j);for(int i=0;i<3;i++)b.step();b.reset();if(b.d.done||b.d.busy)throw std::runtime_error("reset");check(b,j);
 npu_tensor_job cancelled={};cancelled.opcode=13;cancelled.n=128;cancelled.a=0x1000;cancelled.b=0x50000;cancelled.c=0xa0000;cancelled.out=0x100000;cancelled.state=0x110000;cancelled.param0=1073741824;cancelled.param1=536870912;
 before=b.mem;auto priorwrites=b.writes;b.start(cancelled);for(unsigned i=0;i<300;i++)b.step();if(!b.d.busy)throw std::runtime_error("stateful cancellation did not run");b.reset();for(unsigned i=0;i<12;i++)b.step();if(b.d.done||b.d.busy||b.d.mem_req_valid||b.writes!=priorwrites||b.mem!=before)throw std::runtime_error("cancelled state published");tests++;
 b.d.rsp_ready=0;b.d.req_valid=1;b.d.req_write=0;b.d.req_addr=0x10006010;b.step();b.d.req_valid=0;auto held=b.d.rsp_rdata;for(unsigned i=0;i<4;i++){b.step();if(!b.d.rsp_valid||b.d.rsp_rdata!=held||b.d.rsp_error||b.d.req_ready)throw std::runtime_error("MMIO response backpressure");}b.d.rsp_ready=1;b.step();tests++;
 for(int file=1;file<argc;file++)fixtures(b,argv[file]);
   printf("TENSOR_V2_CORE_PASS cases=%u reads=%u writes=%u cycles=%u\n",tests,b.reads,b.writes,b.ticks);
  pipeline_boundaries(b);
  conv_tap_benchmark(b);
  raw_stream_benchmark(b);
  delta_column_benchmark(b);
  norm_benchmark(b);
  norm_extremes(b);
  vector_prefetch_benchmark(b);
printf("TENSOR_V2_PASS cases=%u reads=%u writes=%u cycles=%u\n",tests,b.reads,b.writes,b.ticks);return 0;
 }catch(const std::exception&e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
