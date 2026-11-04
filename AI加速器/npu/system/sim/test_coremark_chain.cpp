#include "Vag_npu_tensor_sim.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>

struct Request {
    uint32_t address=0,strobe=0;
    bool write=false;
    std::array<uint32_t,8> data{};
    bool operator==(const Request& b) const {return address==b.address&&strobe==b.strobe&&write==b.write&&data==b.data;}
};
int main(int argc,char **argv) {
    Verilated::commandArgs(argc,argv);
    const uint64_t limit=argc>1?std::strtoull(argv[1],nullptr,10):30000000;
    const bool legacy=argc>2&&std::string(argv[2])=="legacy";
    const bool readcard=argc>2&&std::string(argv[2])=="readcard";
    const bool card_present=!(argc>3&&std::string(argv[3])=="no-card");
    Vag_npu_tensor_sim d;
    std::vector<uint8_t> ram(8*1024*1024,0xa5); // external DDR BFM, never expected outputs
    struct Return { uint64_t due; std::array<uint32_t,8> data{}; bool error=false; };
    std::deque<Return> pending; unsigned peak_pending=0;
    unsigned tensor_completions=0,tensor_requests=0,matrix_requests=0,cpu_requests=0; uint64_t overlap=0; bool last_tensor_done=false;
    bool held=false,trapped=false;
    Request held_request;
    const unsigned ddr_outstanding=32;
    uint32_t random=0x32718ab9,reads=0,writes=0,stalls=0,spi_edges=0;
    uint64_t cycles=0;std::string console;
    d.rst=1;d.card_present=card_present;d.ddr_ready=0;d.ddr_fault=0;
    d.mem_req_ready=0;d.mem_rsp_valid=0;d.mem_rsp_error=0;
    for(int i=0;i<8;i++)d.mem_rsp_data[i]=0;
    auto tick=[&]() {
        random=(random>>1)^((0u-(random&1u))&0xd0000001u);
        d.ddr_ready=!d.rst&&cycles>20;
        d.mem_req_ready=!d.rst&&pending.size()<ddr_outstanding&&((random&3u)!=0);
        d.mem_rsp_valid=!d.rst&&!pending.empty()&&pending.front().due<=cycles;
        d.mem_rsp_error=!pending.empty()&&pending.front().error;
        for(int i=0;i<8;i++) d.mem_rsp_data[i]=pending.empty()?0:pending.front().data[i];
        d.clk=0;d.eval();
        Request q;q.address=d.mem_req_addr;q.strobe=d.mem_req_wstrb;q.write=d.mem_req_write;
        for(int i=0;i<8;i++)q.data[i]=d.mem_req_wdata[i];
        if(held&&(!d.mem_req_valid||!(q==held_request))) throw std::runtime_error("DDR request changed under backpressure");
        held=!d.rst&&d.mem_req_valid&&!d.mem_req_ready;
        if(held){held_request=q;++stalls;}
        bool request=d.mem_req_valid&&d.mem_req_ready;
        bool reply=d.mem_rsp_valid&&d.mem_rsp_ready;
        if(!d.rst){
            tensor_requests+=d.tensor_request;matrix_requests+=d.matrix_request;cpu_requests+=d.cpu_request;
            overlap+=d.tensor_busy&&d.npu_busy;
            if(d.tensor_done&&!last_tensor_done)++tensor_completions;
            last_tensor_done=d.tensor_done;
        }
        bool old_sd_clk=d.sd_clk;
        d.clk=1;d.eval();Verilated::timeInc(1);
        if(!old_sd_clk&&d.sd_clk)++spi_edges;
        if(d.console_valid){char c=char(d.console_data);console+=c;std::putchar(c);}
        if(d.trap_valid){trapped=true;std::fprintf(stderr,"TRAP pc=%08x cause=%u value=%08x\n",d.trap_pc,d.trap_cause,d.trap_value);}
        if(d.rst){pending.clear();held=false;peak_pending=0;}
        else {
            if(reply){if(pending.empty())throw std::runtime_error("DDR ACK without credit");pending.pop_front();}
            if(request){
                if(pending.size()>=ddr_outstanding)throw std::runtime_error("DDR return credit overflow");
                Return response;response.due=cycles+3+(random&7u);
                response.error=(q.address&31u)||uint64_t(q.address)+32>ram.size();
                if(q.write)++writes;else ++reads;
                if(!response.error){
                    if(q.write)for(unsigned b=0;b<32;b++)if(q.strobe&(1u<<b))ram[q.address+b]=uint8_t(q.data[b/4]>>((b%4)*8));
                    for(unsigned b=0;b<32;b++)response.data[b/4]|=uint32_t(ram[q.address+b])<<((b%4)*8);
                }
                pending.push_back(response);peak_pending=std::max(peak_pending,unsigned(pending.size()));
            }
            if((d.npu_done&&d.matrix_outstanding)||(d.tensor_done&&d.tensor_outstanding))
                throw std::runtime_error("NPU done before pending DDR response was consumed");
        }
        d.clk=0;d.eval();Verilated::timeInc(1);
        ++cycles;
    };
    try {
        for(int i=0;i<5;i++)tick();d.rst=0;cycles=0;
        while(cycles<limit&&!d.done&&!Verilated::gotFinish())tick();
    } catch(const std::exception& e) {
        std::fprintf(stderr,"SYSTEM_PROTOCOL_FAILURE %s\n",e.what());return 2;
    }
    std::printf("\nSYSTEM_RESULT {\"done\":%s,\"exit_code\":%u,\"cycles\":%llu,"
        "\"retired\":%llu,\"dual_retired\":%u,\"trap\":%s,\"last_pc\":%u,"
        "\"peripheral_requests\":%u,\"legacy_completions\":%u,\"dma_completions\":%u,"
        "\"ddr_reads\":%u,\"ddr_writes\":%u,\"ddr_backpressure\":%u,\"sd_clock_edges\":%u}\n",
        d.done?"true":"false",d.exit_code,(unsigned long long)cycles,
        (unsigned long long)d.instret_count,d.dual_retire_count,trapped?"true":"false",d.retire_pc,
        d.peripheral_requests,d.legacy_completions,d.dma_completions,reads,writes,stalls,spi_edges);
    std::printf("TENSOR_SYSTEM_BUS {\"tensor_completions\":%u,\"overlap_cycles\":%llu,\"tensor_requests\":%u,\"matrix_requests\":%u,\"cpu_requests\":%u,\"ddr_outstanding\":%u,\"peak_pending\":%u}\n",tensor_completions,(unsigned long long)overlap,tensor_requests,matrix_requests,cpu_requests,ddr_outstanding,peak_pending);
    bool ok=d.done&&!d.exit_code&&!trapped&&pending.empty()&&console.find("MEASURED_CYCLES_HEX=")!=std::string::npos&&console.find("seedcrc          : 0xe9f5")!=std::string::npos&&console.find("[0]crclist       : 0xe714")!=std::string::npos&&console.find("[0]crcmatrix     : 0x1fd7")!=std::string::npos&&console.find("[0]crcstate      : 0x8e3a")!=std::string::npos;
    std::puts(ok?"PASS_SYSTEM_RTL":"FAIL_SYSTEM_RTL");
    return ok?0:1;
}
