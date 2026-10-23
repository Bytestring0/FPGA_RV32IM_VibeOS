// Cycle-accurate randomized-latency bus + independent RV32IM retirement oracle.
// CSR result checking is delegated to trap_tests.S; all integer/memory/branch
// architectural effects are computed independently here.
#include "Vrv32_core.h"
#include "verilated.h"
#include <array>
#include <vector>
#include <fstream>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
static s32 sext(u32 v, unsigned bits) { return s32(v << (32-bits)) >> (32-bits); }
static u32 get32(const std::vector<uint8_t>& m, u32 a) {
    if (u64(a) + 3 >= m.size()) throw std::runtime_error("read outside RAM");
    return u32(m[a]) | u32(m[a+1])<<8 | u32(m[a+2])<<16 | u32(m[a+3])<<24;
}
struct Oracle {
    std::vector<uint8_t> memory;
    std::array<u32,32> r{};
    u32 pc = 0;
    uint64_t retired = 0;
    explicit Oracle(const std::vector<uint8_t>& m): memory(m) {}
    void step(u32 retire_pc, u32 retire_insn, u32 retire_rd, u32 retire_data, unsigned lane) {
        const u32 ins = get32(memory,pc), op = ins&127, rd=(ins>>7)&31;
        const u32 f3=(ins>>12)&7, rs1=(ins>>15)&31, rs2=(ins>>20)&31, f7=ins>>25;
        const u32 a=r[rs1], b=r[rs2], imm=u32(sext(ins>>20,12));
        u32 next=pc+4, value=0, dest=rd;
        auto error=[&](const char* why) {
            char buf[256];
            std::snprintf(buf,sizeof(buf),
                "%s #%llu lane=%u refpc=%08x dutpc=%08x ins=%08x dutins=%08x rd=%u/%u data=%08x/%08x",
                why,(unsigned long long)retired,lane,pc,retire_pc,ins,retire_insn,
                dest,retire_rd,value,retire_data);
            throw std::runtime_error(buf);
        };
        if (pc!=retire_pc || ins!=retire_insn) error("PC/insn mismatch");
        switch(op) {
            case 0x37: value=ins&0xfffff000; break;
            case 0x17: value=pc+(ins&0xfffff000); break;
            case 0x6f: value=pc+4; next=pc+sext(((ins>>31)<<20)|(((ins>>12)&255)<<12)|
                               (((ins>>20)&1)<<11)|(((ins>>21)&1023)<<1),21); break;
            case 0x67: value=pc+4; next=(a+imm)&~1u; break;
            case 0x63: {
                dest=0; bool taken=false;
                switch(f3) {
                    case 0: taken=a==b; break; case 1: taken=a!=b; break;
                    case 4: taken=s32(a)<s32(b); break; case 5: taken=s32(a)>=s32(b); break;
                    case 6: taken=a<b; break; case 7: taken=a>=b; break;
                }
                if(taken) next=pc+sext(((ins>>31)<<12)|(((ins>>7)&1)<<11)|
                                      (((ins>>25)&63)<<5)|(((ins>>8)&15)<<1),13);
                break;
            }
            case 0x03: {
                u32 addr=a+imm;
                if (addr>=memory.size()) { value=retire_data; break; }
                switch(f3) {
                    case 0: value=u32(sext(memory.at(addr),8)); break;
                    case 1: value=u32(sext(memory.at(addr)|(u32(memory.at(addr+1))<<8),16)); break;
                    case 2: value=get32(memory,addr); break;
                    case 4: value=memory.at(addr); break;
                    case 5: value=memory.at(addr)|(u32(memory.at(addr+1))<<8); break;
                }
                break;
            }
            case 0x23: {
                dest=0; u32 addr=a+sext(((ins>>25)<<5)|((ins>>7)&31),12);
                if (addr<memory.size())
                    for(unsigned i=0;i<(1u<<f3);++i) memory.at(addr+i)=uint8_t(b>>(8*i));
                break;
            }
            case 0x13: case 0x33: {
                u32 c=op==0x13?imm:b;
                if(op==0x33 && f7==1) {
                    switch(f3) {
                        case 0: value=u32(uint64_t(a)*b); break;
                        case 1: value=u32(uint64_t(int64_t(s32(a))*int64_t(s32(b)))>>32); break;
                        case 2: value=u32(uint64_t(int64_t(s32(a))*int64_t(uint64_t(b)))>>32); break;
                        case 3: value=u32((uint64_t(a)*uint64_t(b))>>32); break;
                        case 4: value=b==0?~0u:(a==0x80000000 && b==~0u)?a:u32(s32(a)/s32(b)); break;
                        case 5: value=b==0?~0u:a/b; break;
                        case 6: value=b==0?a:(a==0x80000000 && b==~0u)?0:u32(s32(a)%s32(b)); break;
                        case 7: value=b==0?a:a%b; break;
                    }
                } else switch(f3) {
                    case 0: value=(op==0x33 && f7==32)?a-c:a+c; break;
                    case 1: value=a<<(c&31); break;
                    case 2: value=s32(a)<s32(c); break;
                    case 3: value=a<c; break;
                    case 4: value=a^c; break;
                    case 5: value=(ins&(1u<<30))?u32(s32(a)>>(c&31)):a>>(c&31); break;
                    case 6: value=a|c; break;
                    case 7: value=a&c; break;
                }
                // Independent reference implementations of RV32 Zba/Zbb.
                if (op==0x33 && f7==0x10 && (f3==2 || f3==4 || f3==6))
                    value=(a<<(f3/2))+b;
                if (op==0x33 && f7==0x20) {
                    if(f3==4) value=~(a^b);
                    if(f3==6) value=a|~b;
                    if(f3==7) value=a&~b;
                }
                if (op==0x33 && f7==5) {
                    if(f3==4) value=s32(a)<s32(b)?a:b;
                    if(f3==5) value=a<b?a:b;
                    if(f3==6) value=s32(a)>s32(b)?a:b;
                    if(f3==7) value=a>b?a:b;
                }
                if (f7==0x30 && f3==5)
                    value=(a>>(c&31))|(a<<((32-(c&31))&31));
                if (op==0x33 && f7==0x30 && f3==1)
                    value=(a<<(b&31))|(a>>((32-(b&31))&31));
                if (op==0x33 && f7==4 && f3==4 && rs2==0) value=a&0xffff;
                if (op==0x13 && f3==1) {
                    if((ins>>20)==0x600) value=a?__builtin_clz(a):32;
                    if((ins>>20)==0x601) value=a?__builtin_ctz(a):32;
                    if((ins>>20)==0x602) value=__builtin_popcount(a);
                    if((ins>>20)==0x604) value=u32(sext(a,8));
                    if((ins>>20)==0x605) value=u32(sext(a,16));
                }
                if (op==0x13 && f3==5 && (ins>>20)==0x698) value=__builtin_bswap32(a);
                if (op==0x13 && f3==5 && (ins>>20)==0x287) {
                    value=0;
                    for(unsigned byte=0;byte<4;++byte)
                        if((a>>(byte*8))&255) value|=255u<<(byte*8);
                }
                break;
            }
            case 0x0f: dest=0; break;
            case 0x73: value=retire_data; if(!f3) dest=0; break;
            default: error("unsupported oracle opcode");
        }
        if(!dest) value=0;
        if(dest!=retire_rd || value!=retire_data) error("register mismatch");
        if(dest) r[dest]=value;
        r[0]=0; pc=next; ++retired;
    }
};
// I responses contain two words and a per-word fault bit. D uses the low word.
struct Response { bool pending=false; int delay=0; u64 value=0; uint8_t error=0; };
int main(int argc, char** argv) {
    Verilated::commandArgs(argc,argv);
    if(argc<2) { std::fprintf(stderr,"usage: Vrv32_core image.bin [seed] [maxcycles] [no-oracle]\n"); return 2; }
    std::vector<uint8_t> mem(65536,0);
    std::ifstream input(argv[1],std::ios::binary);
    if(!input) return 2;
    input.read(reinterpret_cast<char*>(mem.data()),mem.size());
    Oracle oracle(mem);
    std::mt19937 rng(argc>2?std::stoul(argv[2]):1);
    uint64_t limit=argc>3?std::stoull(argv[3]):10000000;
    const bool compare=argc<5;
    Vrv32_core t;
    t.rst=1; t.irq_software=0; t.irq_timer=0; t.irq_external=0;
    t.i_req_ready=0; t.i_rsp_valid=0; t.i_rsp_error=0;
    t.d_req_ready=0; t.d_rsp_valid=0; t.d_rsp_error=0;
    for(int i=0;i<5;++i) { t.clk=0;t.eval();t.clk=1;t.eval(); }
    t.rst=0;
    Response ir,dr;
    u64 primary_retired=0, second_retired=0;
    bool awaiting_exit=false; u32 exit_code=0;
    bool held=false; u32 held_addr=0,held_data=0,held_strb=0,held_write=0;
    try {
        for(uint64_t cycles=0;cycles<limit;++cycles) {
            t.clk=0;
            t.i_rsp_valid=ir.pending && ir.delay==0; t.i_rsp_data=ir.value; t.i_rsp_error=ir.error;
            t.d_rsp_valid=dr.pending && dr.delay==0; t.d_rsp_data=u32(dr.value); t.d_rsp_error=dr.error;
            t.i_req_ready=(!ir.pending||ir.delay==0) && rng()%4!=0;
            t.d_req_ready=(!dr.pending||dr.delay==0) && rng()%4!=0;
            t.eval();
            if(compare && t.trap_valid) throw std::runtime_error("unexpected trap");
            if(t.retire_second_valid && !t.retire_valid)
                throw std::runtime_error("second retirement without an older instruction");
            if(t.retire_valid) {
                ++primary_retired;
                if(compare) oracle.step(t.retire_pc,t.retire_insn,t.retire_rd,t.retire_data,0);
            }
            // Architectural order matters: lane 1 observes all lane 0 effects.
            if(t.retire_second_valid) {
                ++second_retired;
                if(compare) oracle.step(t.retire_second_pc,t.retire_second_insn,
                                        t.retire_second_rd,t.retire_second_data,1);
            }
            if(held && (!t.d_req_valid || t.d_req_addr!=held_addr ||
                        t.d_req_wdata!=held_data || t.d_req_wstrb!=held_strb || t.d_req_write!=held_write)) {
                std::fprintf(stderr,"cycle=%llu valid=%u addr=%08x/%08x data=%08x/%08x strb=%x/%x write=%u/%u\n",
                    (unsigned long long)cycles,t.d_req_valid,held_addr,t.d_req_addr,
                    held_data,t.d_req_wdata,held_strb,t.d_req_wstrb,held_write,t.d_req_write);
                throw std::runtime_error("data request changed during backpressure");
            }
            held=t.d_req_valid && !t.d_req_ready;
            held_addr=t.d_req_addr; held_data=t.d_req_wdata;
            held_strb=t.d_req_wstrb; held_write=t.d_req_write;
            bool fi=t.i_req_valid&&t.i_req_ready, fd=t.d_req_valid&&t.d_req_ready;
            bool ci=t.i_rsp_valid&&t.i_rsp_ready, cd=t.d_rsp_valid&&t.d_rsp_ready;
            if(ci) ir.pending=false;
            if(cd) dr.pending=false;
            if(ir.pending && ir.delay>0) --ir.delay;
            if(dr.pending && dr.delay>0) --dr.delay;
            if(fi) {
                if(ir.pending) throw std::runtime_error("multiple outstanding I transactions");
                if(t.i_req_addr&3) throw std::runtime_error("unaligned instruction request");
                ir={true,int(rng()%6),0,0};
                for(unsigned word=0;word<2;++word) {
                    const u32 addr=t.i_req_addr+4u*word;
                    if(u64(addr)+3>=mem.size()) ir.error|=uint8_t(1u<<word);
                    else ir.value|=u64(get32(mem,addr))<<(32*word);
                }
            }
            if(fd) {
                if(dr.pending) throw std::runtime_error("multiple outstanding D transactions");
                dr={true,int(rng()%8),0,false};
                if(t.d_req_addr<mem.size()) {
                    dr.value=get32(mem,t.d_req_addr);
                    if(t.d_req_write)
                        for(int b=0;b<4;++b) if(t.d_req_wstrb&(1<<b))
                            mem.at(t.d_req_addr+b)=uint8_t(t.d_req_wdata>>(8*b));
                } else if(t.d_req_addr==0x10000004 && t.d_req_write) {
                    awaiting_exit=true; exit_code=t.d_req_wdata;
                } else if(t.d_req_addr==0x10000000) {
                    dr.value=1;
                } else dr.error=true;
            }
            t.clk=1;t.eval();
            if(awaiting_exit && cd) {
                std::printf("exit=%u cycles=%llu retired_checked=%llu primary_retired=%llu "
                            "second_retired=%llu dual_retire_count=%u seed=%s\n",exit_code,
                            (unsigned long long)cycles,(unsigned long long)oracle.retired,
                            (unsigned long long)primary_retired,(unsigned long long)second_retired,
                            t.dual_retire_count,argc>2?argv[2]:"1");
                return exit_code?1:0;
            }
        }
        throw std::runtime_error("timeout");
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL %s\n",e.what()); return 1;
    }
}
