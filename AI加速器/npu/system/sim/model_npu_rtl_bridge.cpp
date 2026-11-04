#include "Vmodel_npu_rtl.h"
#include "verilated.h"
extern "C" {
#include "../sw/npu_tensor_driver.h"
#include "../integration/agent_runtime_20261101/sw/model/vendor/npu_v1/npu_raw.h"
}
#include <deque>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <cstdlib>
static uint64_t opcode_cycles[23]={},opcode_calls[23]={},read_lines[2]={},write_lines[2]={};
static unsigned char *memory_base;static size_t memory_bytes;static uint32_t memory_offset;
static uint64_t clocks=0,matrix_clocks=0,tensor_clocks=0,matrix_jobs=0,tensor_jobs=0;
static npu_raw_job deferred;static bool deferred_valid=false;
struct Rtl {
 Vmodel_npu_rtl d;
 struct Response{uint64_t due;std::array<uint32_t,8> data{};bool error=false;};
 std::deque<Response> replies;
 unsigned memory_depth=16,memory_delay=2;
 Rtl(){if(const char*p=std::getenv("QW_RTL_MEMORY_DEPTH"))memory_depth=std::strtoul(p,0,10);if(const char*p=std::getenv("QW_RTL_MEMORY_DELAY"))memory_delay=std::strtoul(p,0,10);d.rsp_ready=1;d.rst=1;step();step();d.rst=0;step();}
 void step(){
  d.clk=0;d.mem_req_ready=replies.size()<memory_depth&&clocks%5!=1;
  d.mem_rsp_valid=!replies.empty()&&replies.front().due<=clocks;
  d.mem_rsp_error=d.mem_rsp_valid&&replies.front().error;
  for(int q=0;q<8;q++)d.mem_rsp_data[q]=d.mem_rsp_valid?replies.front().data[q]:0;
  d.eval();bool take=d.mem_req_valid&&d.mem_req_ready,ack=d.mem_rsp_valid&&d.mem_rsp_ready;
  if(ack)replies.pop_front();
  if(take){if(d.mem_req_write)++write_lines[d.select_tensor];else ++read_lines[d.select_tensor];Response response;response.due=clocks+memory_delay;uint32_t a=d.mem_req_addr;
   response.error=a<memory_offset||uint64_t(a-memory_offset)+32>memory_bytes;
   if(!response.error){auto p=memory_base+(a-memory_offset);
    if(d.mem_req_write){for(unsigned b=0;b<32;b++)if((d.mem_req_wstrb>>b)&1)p[b]=d.mem_req_wdata[b/4]>>(8*(b%4));}
    else std::memcpy(response.data.data(),p,32);
   }replies.push_back(response);
  }
  d.clk=1;d.eval();++clocks;
 }
 uint32_t csr(uint32_t off,bool wr=false,uint32_t value=0){
  d.req_addr=(d.select_tensor?0x10006000:0x10004000)+off;d.req_write=wr;d.req_wdata=value;d.req_wstrb=15;d.req_valid=1;
  unsigned wait=0;while(!d.req_ready){step();if(++wait>100)throw std::runtime_error("MMIO ready");}
  step();d.req_valid=0;while(!d.rsp_valid){step();if(++wait>200)throw std::runtime_error("MMIO response");}
  if(d.rsp_error)throw std::runtime_error("MMIO error");auto result=d.rsp_rdata;step();return result;
 }
 void finish(){uint64_t begin=clocks;while(csr(4)&1){if(clocks-begin>100000000)throw std::runtime_error("NPU completion timeout");}
  if(!replies.empty())throw std::runtime_error("NPU completed before DDR ACK");}
};
static Rtl &rtl(){static Rtl r;return r;}
extern "C" int __wrap_npu_tensor_init(npu_tensor_context*c,const npu_tensor_config*cfg){if(!c||!cfg)return -200;*c={};c->config=*cfg;memory_base=cfg->mapping.data;memory_bytes=cfg->mapping.capacity;memory_offset=cfg->mapping.ddr_base;return 0;}
extern "C" int __real_npu_raw_reference(const npu_raw_job*,const npu_raw_layout*,const npu_raw_region*,size_t,const npu_raw_mapping*);
extern "C" int __wrap_npu_raw_reference(const npu_raw_job*j,const npu_raw_layout*l,const npu_raw_region*regions,size_t count,const npu_raw_mapping*m){
 if(!memory_base)return __real_npu_raw_reference(j,l,regions,count,m);
 if(deferred_valid)return -3;deferred=*j;deferred_valid=true;(void)l;return 0;
}
extern "C" int __wrap_npu_tensor_execute(npu_tensor_context*,const npu_tensor_job*j,npu_tensor_stats*s){
 try{
 auto&r=rtl();npu_tensor_stats stats={};
 if(j->opcode==19&&deferred_valid){
  const auto &a=deferred;r.d.select_tensor=0;r.csr(0,true,2);
  r.csr(8,true,a.rows);r.csr(12,true,a.cols);r.csr(16,true,a.weight_offset);r.csr(20,true,0);
  r.csr(24,true,a.activation_offset);r.csr(28,true,0);r.csr(32,true,j->out);r.csr(36,true,0);
  r.csr(40,true,a.weight_stride);r.csr(44,true,a.activation_stride);r.csr(48,true,4);
  r.csr(0x50,true,j->b);r.csr(0x54,true,j->param0);r.csr(0x4c,true,1);r.csr(0,true,1);r.finish();
  stats.error=r.csr(0x34);matrix_clocks+=r.csr(0x38);++matrix_jobs;
  if(r.csr(0x3c)!=a.rows)throw std::runtime_error("matrix row ACK count");
  stats.numeric.status=r.csr(0x58);stats.numeric.saturations=r.csr(0x5c);stats.numeric.underflows=r.csr(0x60);stats.numeric.invalid_inputs=r.csr(0x64);
  r.csr(0x4c,true,0);deferred_valid=false;
 }else{
  if(deferred_valid)throw std::runtime_error("raw matrix must be followed by exact reduce19");
  r.d.select_tensor=1;r.csr(0,true,2);const uint32_t*p=(const uint32_t*)j;
  for(unsigned q=0;q<16;q++)r.csr(32+4*q,true,p[q]);r.csr(0,true,1);r.finish();
  stats.error=r.csr(8);auto elapsed=r.csr(12);tensor_clocks+=elapsed;++tensor_jobs;opcode_cycles[j->opcode]+=elapsed;++opcode_calls[j->opcode];
  stats.result=r.csr(24);stats.numeric.saturations=r.csr(28);stats.numeric.status=r.csr(96);
  stats.numeric.domain_clamps=r.csr(100);stats.numeric.underflows=r.csr(104);stats.numeric.zero_vectors=r.csr(108);stats.numeric.invalid_inputs=r.csr(112);
 }
 if(s)*s=stats;
 if(stats.error){std::fprintf(stderr,"FULL_MODEL_RTL_ERROR op%u error%u\n",j->opcode,stats.error);return -203;}
 if((tensor_jobs+matrix_jobs)%500==0){std::printf("FULL_MODEL_RTL_PROGRESS matrix=%llu tensor=%llu cycles=%llu\n",(unsigned long long)matrix_jobs,(unsigned long long)tensor_jobs,(unsigned long long)clocks);std::fflush(stdout);}
 return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FULL_MODEL_RTL_FAIL %s\n",e.what());return -203;}
}
struct Summary {~Summary(){for(unsigned op=0;op<23;++op)if(opcode_calls[op])std::printf("OPCODE_CYCLES op=%u jobs=%llu cycles=%llu\n",op,(unsigned long long)opcode_calls[op],(unsigned long long)opcode_cycles[op]);for(unsigned unit=0;unit<2;++unit)std::printf("MEMORY_LINES tensor=%u reads=%llu writes=%llu\n",unit,(unsigned long long)read_lines[unit],(unsigned long long)write_lines[unit]);std::printf("FULL_MODEL_RTL_TOTAL matrix_jobs=%llu tensor_jobs=%llu matrix_cycles=%llu tensor_cycles=%llu bus_clocks=%llu host_scheduler=true cpu_rtl=false board=false\n",(unsigned long long)matrix_jobs,(unsigned long long)tensor_jobs,(unsigned long long)matrix_clocks,(unsigned long long)tensor_clocks,(unsigned long long)clocks);}};static Summary summary;
