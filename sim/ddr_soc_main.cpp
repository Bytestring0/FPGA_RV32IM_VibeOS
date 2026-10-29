#include "Vqinglong_soc.h"
#include "verilated.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Vqinglong_soc top;
    std::unordered_map<uint32_t,uint32_t> memory;
    std::array<uint32_t,8> response{};
    bool pending=false; unsigned delay=0, transactions=0;
    top.clk=0; top.rst=1; top.irq_external=0; top.ddr_ready=1; top.ddr_fault=0;
    top.mem_req_ready=0; top.mem_rsp_valid=0; top.mem_rsp_error=0;
    for(int k=0;k<8;k++) { top.clk=0; top.eval(); top.clk=1; top.eval(); }
    top.rst=0;
    for(uint64_t cycle=0;cycle<3000000;cycle++) {
        top.clk=0;
        top.mem_req_ready=!pending && cycle%5!=0;
        top.mem_rsp_valid=pending && !delay;
        for(int k=0;k<8;k++) top.mem_rsp_data[k]=response[k];
        top.eval();
        if(top.trap_valid) std::fprintf(stderr,"PRETRAP pc=%08x cause=%u addr=%08x cycle=%llu\n",top.trap_pc,top.trap_cause,top.trap_value,(unsigned long long)cycle);
        if(top.mem_rsp_valid && top.mem_rsp_ready) pending=false;
        if(top.mem_req_valid && top.mem_req_ready) {
            if(top.mem_req_addr & 31) { std::fprintf(stderr,"Unaligned line\n"); return 3; }
            uint32_t addr=top.mem_req_addr>>2;
            for(int k=0;k<8;k++) {
                auto &word=memory[addr+k];
                if(top.mem_req_write) for(int b=0;b<4;b++) {
                    if((top.mem_req_wstrb>>(4*k+b))&1) {
                        uint32_t mask=255u<<(8*b);
                        word=(word & ~mask) | (top.mem_req_wdata[k] & mask);
                    }
                }
                response[k]=word;
            }
            pending=true; delay=1+cycle%17; transactions++;
        } else if(delay) delay--;
        top.clk=1; top.eval();

        if(top.console_valid) std::putchar(top.console_data);
        if(top.trap_valid) std::fprintf(stderr,"TRAP pc=%08x cause=%u addr=%08x cycle=%llu\n",top.trap_pc,top.trap_cause,top.trap_value,(unsigned long long)cycle);
        if(top.done) {
            std::printf("DDR SoC simulation exit=%u cycles=%llu transactions=%u\n",top.exit_code,
                static_cast<unsigned long long>(cycle),transactions);
            return top.exit_code ? 1 : 0;
        }
    }
    std::fprintf(stderr,"DDR SoC timeout pc=%08x cause=%u\n",top.retire_pc,top.trap_cause);
    return 2;
}
