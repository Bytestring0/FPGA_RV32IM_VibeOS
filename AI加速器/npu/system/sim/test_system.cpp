#include "Vag_npu_sim.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

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
    Vag_npu_sim d;
    std::vector<uint8_t> ram(8*1024*1024,0xa5); // external DDR BFM, never expected outputs
    bool pending=false,response_error=false,held=false,trapped=false;
    Request held_request;
    std::array<uint32_t,8> response{};
    uint32_t latency=0,random=0x32718ab9,reads=0,writes=0,stalls=0,spi_edges=0;
    uint64_t cycles=0;std::string console;
    d.rst=1;d.card_present=card_present;d.ddr_ready=0;d.ddr_fault=0;
    d.mem_req_ready=0;d.mem_rsp_valid=0;d.mem_rsp_error=0;
    for(int i=0;i<8;i++)d.mem_rsp_data[i]=0;
    auto tick=[&]() {
        random=(random>>1)^((0u-(random&1u))&0xd0000001u);
        d.ddr_ready=!d.rst&&cycles>20;
        d.mem_req_ready=!d.rst&&!pending&&((random&3u)!=0);
        d.mem_rsp_valid=!d.rst&&pending&&latency==0;
        d.mem_rsp_error=response_error;
        for(int i=0;i<8;i++) d.mem_rsp_data[i]=response[i];
        d.clk=0;d.eval();
        Request q;q.address=d.mem_req_addr;q.strobe=d.mem_req_wstrb;q.write=d.mem_req_write;
        for(int i=0;i<8;i++)q.data[i]=d.mem_req_wdata[i];
        if(held&&(!d.mem_req_valid||!(q==held_request))) throw std::runtime_error("DDR request changed under backpressure");
        held=!d.rst&&d.mem_req_valid&&!d.mem_req_ready;
        if(held){held_request=q;++stalls;}
        bool request=d.mem_req_valid&&d.mem_req_ready;
        bool reply=d.mem_rsp_valid&&d.mem_rsp_ready;
        bool old_sd_clk=d.sd_clk;
        d.clk=1;d.eval();Verilated::timeInc(1);
        if(!old_sd_clk&&d.sd_clk)++spi_edges;
        if(d.console_valid){char c=char(d.console_data);console+=c;std::putchar(c);}
        if(d.trap_valid){trapped=true;std::fprintf(stderr,"TRAP pc=%08x cause=%u value=%08x\n",d.trap_pc,d.trap_cause,d.trap_value);}
        if(d.rst){pending=false;held=false;latency=0;}
        else {
            if(reply)pending=false;
            else if(pending&&latency)--latency;
            if(request){
                if(pending)throw std::runtime_error("more than one downstream transaction");
                pending=true;latency=2+(random&7u);
                response_error=(q.address&31u)||uint64_t(q.address)+32>ram.size();
                response.fill(0);
                if(q.write)++writes;else ++reads;
                if(!response_error){
                    if(q.write)for(unsigned b=0;b<32;b++)if(q.strobe&(1u<<b))ram[q.address+b]=uint8_t(q.data[b/4]>>((b%4)*8));
                    for(unsigned b=0;b<32;b++)response[b/4]|=uint32_t(ram[q.address+b])<<((b%4)*8);
                }
            }
            if(d.npu_done&&pending&&d.ddr_transaction_owner)
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
    bool ok=d.done&&!d.exit_code&&!trapped&&d.instret_count>100;
    if(readcard)ok=ok&&d.dma_completions==1&&reads>0&&writes>0&&spi_edges>1024&&
                      console.find("PASS_SD_FAT32_DDR_NPU")!=std::string::npos;
    else if(legacy)ok=ok&&d.legacy_completions==9&&console.find("PASS_NPU_FIRMWARE")!=std::string::npos;
    else ok=ok&&d.dma_completions==140&&reads>100&&writes>100&&spi_edges>1024&&
                 console.find("PASS_AG_NPU_SYSTEM")!=std::string::npos;
    std::puts(ok?"PASS_SYSTEM_RTL":"FAIL_SYSTEM_RTL");
    return ok?0:1;
}
