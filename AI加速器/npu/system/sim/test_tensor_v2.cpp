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
 void step(){d.clk=0; d.mem_req_ready=!pending&&(ticks%5!=1);d.mem_rsp_valid=pending&&delay==0;d.mem_rsp_error=fault;for(int i=0;i<8;i++)d.mem_rsp_data[i]=data[i];d.eval();if(held&&!d.rst){if(!d.mem_req_valid||d.mem_req_addr!=heldaddr||bool(d.mem_req_write)!=heldwrite||d.mem_req_wstrb!=heldstrobe)throw std::runtime_error("DDR request changed under backpressure");for(int q=0;q<8;q++)if(d.mem_req_wdata[q]!=helddata[q])throw std::runtime_error("DDR payload changed under backpressure");}held=!d.rst&&d.mem_req_valid&&!d.mem_req_ready;if(held){heldaddr=d.mem_req_addr;heldwrite=d.mem_req_write;heldstrobe=d.mem_req_wstrb;for(int q=0;q<8;q++)helddata[q]=d.mem_req_wdata[q];}bool take=d.mem_req_valid&&d.mem_req_ready;bool rsp=d.mem_rsp_valid&&d.mem_rsp_ready; if(take){addr=d.mem_req_addr;if(addr+32>mem.size())throw std::runtime_error("DDR outside test memory");fault=d.mem_req_write?inject_write:inject_read;if(d.mem_req_write){writes++;if(!fault)for(int b=0;b<32;b++)if((d.mem_req_wstrb>>b)&1)mem[addr+b]=(d.mem_req_wdata[b/4]>>(8*(b%4)))&255;}else{reads++;memcpy(data,mem.data()+addr,32);}pending=true;delay=2+(ticks%3);}else if(rsp)pending=false;else if(pending&&delay)delay--;d.clk=1;d.eval();ticks++;}
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
static void pipeline_boundaries(Bench &b){
 b.mem.resize(2<<20,0xa5);npu_tensor_job j{};j.a=0x1000;j.b=0x50000;j.c=0xa0000;j.out=0x100000;j.state=0x110000;
 unsigned tails[]={1,3,7,8,9,15,31,32,33,63};
 for(unsigned n:tails){
  j.opcode=15;j.n=n;
  for(unsigned i=0;i<n;i++)put(b.mem,j.a+4*i,int32_t(i*65537)-1000000);
  check_all_stats(b,j);
  j.opcode=1;check_all_stats(b,j);
 }
 // Multiple clipping stages from a single stateful update must all count.
 j.opcode=20;j.n=1;j.param0=1073741824;j.param1=1073741824;
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
printf("TENSOR_V2_PASS cases=%u reads=%u writes=%u cycles=%u\n",tests,b.reads,b.writes,b.ticks);return 0;
 }catch(const std::exception&e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
