#ifndef NPU_DMA_LANES
#define NPU_DMA_LANES 16
#endif
#ifndef NPU_WEIGHT_DEPTH
#define NPU_WEIGHT_DEPTH 4
#endif
#if 1
// Real DDR line BFM: stalled requests, delayed acknowledgements, injected faults.
#include "Vnpu_dma_top.h"
#include "Vnpu_dma_top___024root.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
static_assert(NPU_DMA_LANES==16 || NPU_DMA_LANES==32 || NPU_DMA_LANES==64,"supported lane counts");

static constexpr uint32_t C=0x10004000u, DDR_BYTES=0x40000000u;
struct Tb {
    struct Fixture {
        unsigned n,k,seed,groups,ws,os;
        uint32_t wb,ab,ob;
    };
    struct Transfer {
        uint64_t due;
        uint32_t addr,strobe;
        bool write,error;
        std::array<uint32_t,8> data{};
    };
    struct Request {
        uint32_t addr,strobe; bool write;
        std::array<uint32_t,8> data;
    };
    unsigned access_unit=4;
    Vnpu_dma_top d;
    std::vector<uint8_t> mem=std::vector<uint8_t>(1u<<20,0);
    std::map<uint32_t,uint8_t> high_mem;
    std::deque<Transfer> pending;
    uint64_t cyc=0,reads=0,writes=0,accepted=0,task_committed=0,core_beats=0;
    unsigned checks=0,read_delay=2,write_delay=2;
    unsigned response_limit=1024;
    bool stalled=false,force_stall=false,fault_armed=false,fault_write=false;
    bool check_done=false,verified_done=false;
    uint32_t fault_addr=0;
    unsigned faults_fired=0,max_pending=0;
    uint64_t overlap_responses=0;
    Request held{};
    Fixture expected{};

    Tb() { reset(); }
    void need(bool ok,const std::string &message) {
        ++checks; if(!ok) throw std::runtime_error(message);
    }
    unsigned state() const { return d.rootp->npu_dma_top__DOT__state; }
    static int activation(unsigned i,unsigned seed) { return int((i*13+seed)%255)-127; }
    static int weight(unsigned r,unsigned i,unsigned seed) { return int((r*11+i*7+seed)%16)-8; }
    uint8_t peek(uint32_t addr) const {
        if(addr<mem.size()) return mem[addr];
        auto it=high_mem.find(addr); return it==high_mem.end()?0:it->second;
    }
    void store(uint32_t addr,uint8_t value) {
        need(addr<DDR_BYTES,"model DDR write outside 1 GiB");
        if(addr<mem.size()) mem[addr]=value; else high_mem[addr]=value;
    }
    uint32_t get32(uint32_t addr) const {
        uint32_t value=0; for(unsigned b=0;b<4;++b) value|=uint32_t(peek(addr+b))<<(8*b);
        return value;
    }
    Request request() const {
        Request r{}; r.addr=d.mem_req_addr;r.strobe=d.mem_req_wstrb;r.write=d.mem_req_write;
        for(unsigned i=0;i<8;++i) r.data[i]=d.mem_req_wdata[i];
        return r;
    }
    void verify(const Fixture &f) {
        for(unsigned row=0;row<f.n;++row) {
            for(unsigned group=0;group<f.groups;++group) {
                int64_t sum=0;
                for(unsigned j=0;j<128;++j) {
                    unsigned x=group*128+j;
                    if(x<f.k) sum+=int64_t(weight(row,x,f.seed))*activation(x,f.seed);
                }
                uint32_t p=f.ob+row*f.os+group*8;
                need(get32(p)==(row|(group<<16)),"DDR row/group tag mismatch");
                need(int32_t(get32(p+4))==sum,"DDR signed INT32 partial mismatch");
            }
            for(unsigned b=f.groups*8;b<f.os;++b)
                need(peek(f.ob+row*f.os+b)==0xcd,"masked write changed row padding");
        }
        for(unsigned b=0;b<32;++b) {
            need(peek(f.ob-32+b)==0xcd,"write changed preceding guard");
            uint64_t after=uint64_t(f.ob)+uint64_t(f.n)*f.os+b;
            if(after<DDR_BYTES) need(peek(uint32_t(after))==0xcd,"write changed trailing guard");
        }
    }
    void step() {
        d.clk=0;
        if(d.rst) { pending.clear(); stalled=false; }
        d.mem_req_ready=!force_stall && pending.size()<response_limit && ((cyc%5)!=1);
        d.mem_rsp_valid=!d.rst && !pending.empty() && pending.front().due<=cyc;
        d.mem_rsp_error=d.mem_rsp_valid && pending.front().error;
        if(d.mem_rsp_valid) for(unsigned i=0;i<8;++i) d.mem_rsp_data[i]=pending.front().data[i];
        d.eval();
        bool old_busy=d.busy,old_done=d.done;
        unsigned old_state=state(),old_beat=d.rootp->npu_dma_top__DOT__beat_idx;
        bool req_hs=d.mem_req_valid&&d.mem_req_ready;
        bool rsp_hs=d.mem_rsp_valid&&d.mem_rsp_ready;
        bool write_ack=false;
        if(stalled && !d.rst) {
            Request now=request();
            need(d.mem_req_valid && now.addr==held.addr && now.strobe==held.strobe &&
                 now.write==held.write && now.data==held.data,"DDR request changed under backpressure");
        }
        stalled=!d.rst && d.mem_req_valid&&!d.mem_req_ready;
        if(stalled) held=request();
        if(rsp_hs) {
            Transfer r=pending.front(); pending.pop_front();
            if(!r.write && old_state==6) ++overlap_responses;
            if(r.write&&!r.error) {
                unsigned bytes=0;
                for(unsigned b=0;b<32;++b) if(r.strobe&(uint32_t(1)<<b)) {
                    store(r.addr+b,uint8_t(r.data[b/4]>>((b%4)*8))); ++bytes;
                }
                need(bytes && bytes%(access_unit)==0,"write strobe does not contain complete records");
                task_committed+=bytes/access_unit; write_ack=true;
            }
        }
        if(req_hs) {
            Request q=request();
            need(pending.size()<NPU_WEIGHT_DEPTH,"DDR reads exceed configured window");
            need(!q.write || pending.empty(),"write issued before read responses drained");
            for(const auto &p:pending) need(!p.write,"read issued while write outstanding");
            need((q.addr&31)==0 && uint64_t(q.addr)+32<=DDR_BYTES,"unaligned/out-of-range DDR request");
            Transfer r{};r.addr=q.addr;r.write=q.write;r.strobe=q.strobe;r.data=q.data;
            r.due=cyc+(q.write?write_delay:read_delay);
            r.error=fault_armed && q.addr==fault_addr && q.write==fault_write;
            if(r.error) { fault_armed=false; ++faults_fired; }
            if(q.write) ++writes;
            else {
                ++reads; need(q.strobe==0,"read request has write mask");
                for(unsigned i=0;i<8;++i) r.data[i]=get32(q.addr+i*4);
            }
            ++accepted; pending.push_back(r);
            if(pending.size()>max_pending) max_pending=unsigned(pending.size());
        }
        d.clk=1;d.eval();d.clk=0;d.eval();++cyc;
        if(!d.rst && old_busy && old_state==6 &&
           (state()!=6 || d.rootp->npu_dma_top__DOT__beat_idx!=old_beat)) ++core_beats;
        if(d.rst || (!old_busy&&d.busy)) task_committed=0;
        if(!d.rst && d.done&&!old_done) {
            need(write_ack,"DONE preceded the final successful write acknowledgement");
            need(!d.busy&&!d.error,"DONE has busy/error asserted");
            need(d.result_count==task_committed,"DONE result_count differs from committed DDR inventory");
            if(check_done) { verify(expected); verified_done=true; }
        }
    }
    void reset() {
        check_done=false;verified_done=false;fault_armed=false;force_stall=false;
        d.req_valid=0;d.rsp_ready=0;d.rst=1;step();step();
        need(!d.mem_req_valid&&!d.mem_rsp_ready&&!d.req_ready,"bus handshake active during reset");
        d.rst=0;step();
        need(!d.busy&&!d.done&&!d.error&&!d.result_count&&!d.cycle_count,"reset left active status");
    }
    uint32_t access(uint32_t off,bool write=false,uint32_t value=0,unsigned hold=0,
                    bool expect_error=false,unsigned strobe=15) {
        d.req_addr=C+off;d.req_write=write;d.req_wdata=value;d.req_wstrb=strobe;
        d.req_valid=1;d.rsp_ready=0;d.eval();unsigned guard=0;
        while(!d.req_ready) { step();need(++guard<1000,"MMIO request timeout"); }
        step();d.req_valid=0;d.eval();
        while(!d.rsp_valid) { step();need(++guard<1000,"MMIO response timeout"); }
        uint32_t out=d.rsp_rdata;bool error=d.rsp_error;
        for(unsigned i=0;i<hold;++i) {
            // Present an additional request while the first response is held.
            d.req_addr=C+0x80;d.req_write=1;d.req_wdata=0x12345678;d.req_valid=1;d.eval();
            need(!d.req_ready,"MMIO accepted another request while response blocked");
            step();need(d.rsp_valid&&d.rsp_rdata==out&&bool(d.rsp_error)==error,
                        "MMIO response changed under backpressure");
        }
        d.req_valid=0;d.rsp_ready=1;step();d.rsp_ready=0;d.eval();
        need(!d.rsp_valid,"duplicate MMIO response");
        need(error==expect_error,"unexpected MMIO error at offset "+std::to_string(off));
        return out;
    }
    void clear() {
        need(!d.busy,"attempt to clear while task active");access(0,true,2);
        need(!d.done&&!d.error&&!d.error_code&&!d.result_count&&!d.cycle_count,"CLEAR did not reset sticky status");
    }
    void configure(unsigned n,unsigned k,uint32_t wb=0x1000,uint32_t ab=0x8000,uint32_t ob=0x10000) {
        access(8,true,n);access(12,true,k);
        access(16,true,wb);access(20,true,0);access(24,true,ab);access(28,true,0);
        access(32,true,ob);access(36,true,0);
        access(40,true,0);access(44,true,0);access(48,true,0);
    }
    Fixture fixture(unsigned n,unsigned k,unsigned seed) const {
        Fixture f{};f.n=n;f.k=k;f.seed=seed;f.groups=(k+127)/128;
        f.ws=f.groups*64;f.os=((f.groups+3)/4)*32;f.wb=0x1000;
        f.ab=(f.wb+n*f.ws+0x101f)&~31u;
        f.ob=(f.ab+((k+31)/32)*32+0x101f)&~31u;
        return f;
    }
    void load(const Fixture &f) {
        for(unsigned r=0;r<f.n;++r) for(unsigned i=0;i<f.groups*128;i+=2) {
            int lo=i<f.k?weight(r,i,f.seed):7;
            int hi=i+1<f.k?weight(r,i+1,f.seed):7;
            store(f.wb+r*f.ws+i/2,uint8_t((lo&15)|((hi&15)<<4)));
        }
        // Poison padded data: a missing tail mask must fail the golden comparison.
        for(unsigned i=0;i<f.groups*128;++i)
            store(f.ab+i,uint8_t(i<f.k?activation(i,f.seed):127));
        for(unsigned i=0;i<32;++i) store(f.ob-32+i,0xcd);
        for(unsigned r=0;r<f.n;++r) for(unsigned b=0;b<f.os;++b) store(f.ob+r*f.os+b,0xcd);
        for(unsigned b=0;b<32;++b) {
            uint64_t a=uint64_t(f.ob)+uint64_t(f.n)*f.os+b;
            if(a<DDR_BYTES) store(uint32_t(a),0xcd);
        }
    }

    void await_idle(unsigned limit=2000000) {
        unsigned i=0;while(d.busy){step();need(++i<limit,"timeout state="+std::to_string(state()));}
        need(pending.empty(),"undrained DDR responses");
    }
    struct Numeric {uint32_t flags=0,sat=0,under=0;};
    static int64_t rne(__int128 x,uint64_t den,Numeric &s) {
        bool neg=x<0;unsigned __int128 a=neg?-x:x,q=a/den,r=a%den;
        if(r>den-r || (r==den-r && (q&1)))++q;
        if(a && !q){s.flags|=4;s.under++;}
        return neg?-int64_t(q):int64_t(q);
    }
    static int32_t narrow(int64_t v,Numeric &s) {
        if(v>INT32_MAX||v<INT32_MIN){s.flags|=2;s.sat++;return v<0?INT32_MIN:INT32_MAX;}
        return int32_t(v);
    }
    static int32_t coef(uint16_t bits,uint32_t maximum,Numeric &s) {
        unsigned e=(bits>>10)&31;uint64_t m=(bits&1023)|(e?1024:0);
        int sh=e?int(e)-11:-10;__int128 num=__int128(m)*maximum;uint64_t den=127;
        if(sh>=0)num<<=sh;else den<<=-sh;
        return narrow(rne(num,den,s),s);
    }
    void put16(uint32_t a,uint16_t x){store(a,uint8_t(x));store(a+1,uint8_t(x>>8));}
    uint32_t scale_base(const Fixture &f){return (f.ob+f.n*f.os+127)&~31u;}
    static uint16_t scale(unsigned r,unsigned g,unsigned seed) {
        static const uint16_t edge[]={1,0x3ff,0x400,0x401,0x7bf,0x1800,0x2800,0x3400,0x3c00,0x3fff,0x4000,0x7bff};
        return edge[(r*7+g*5+seed)%12];
    }
    void prepare(const Fixture &f,uint32_t maximum,unsigned scale_bias=0) {
        clear();load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
        access(0x4c,true,1);access(0x50,true,scale_base(f)+scale_bias);access(0x54,true,maximum);
        for(unsigned r=0;r<f.n;r++)for(unsigned g=0;g<f.groups;g++)
            put16(scale_base(f)+scale_bias+(r*f.groups+g)*2,scale(r,g,f.seed));
        for(unsigned b=0;b<f.n*4+32;b++)store(f.ob+b,0xcd);
    }
    void run_fused(unsigned n,unsigned k,uint32_t maximum,unsigned seed,unsigned bias=0) {
        Fixture f=fixture(n,k,seed);prepare(f,maximum,bias);
        Numeric expected_status;std::vector<int32_t> expected;
        for(unsigned r=0;r<n;r++) {
            int64_t acc=0;
            for(unsigned g=0;g<f.groups;g++) {
                int32_t partial=0;
                for(unsigned j=0;j<128 && g*128+j<k;j++)
                    partial+=weight(r,g*128+j,seed)*activation(g*128+j,seed);
                int32_t coefficient=coef(scale(r,g,seed),maximum,expected_status);
                acc+=rne(__int128(partial)*coefficient,16384,expected_status);
            }
            expected.push_back(narrow(acc,expected_status));
        }
        uint64_t r0=reads,w0=writes;access(0,true,1);
        access(0x50,true,scale_base(f),0,true);await_idle();
        need(d.done&&!d.error,"fused task failed code="+std::to_string(d.error_code));
        need(d.result_count==n,"fused ACK row count");
        for(unsigned r=0;r<n;r++)need(int32_t(get32(f.ob+r*4))==expected[r],"Q16 result mismatch row="+std::to_string(r));
        for(unsigned b=0;b<32;b++) {
            need(peek(f.ob-32+b)==0xcd,"preceding guard changed");
            need(peek(f.ob+n*4+b)==0xcd,"tail byte strobe changed guard");
        }
        need(access(0x58)==expected_status.flags,"numeric flags mismatch");
        need(access(0x5c)==expected_status.sat,"saturation count mismatch");
        need(access(0x60)==expected_status.under,"underflow count mismatch");
        need(access(0x64)==0,"unexpected invalid count");
        need(writes-w0==(n+7)/8,"fused eight-row output packing");
        uint64_t scale_lines=(bias%32+n*f.groups*2+31)/32;
        need(reads-r0==(k+31)/32+uint64_t(n)*f.groups*2+scale_lines,"single-line scale cache inventory");
        std::printf("PERF_FUSED lanes=%u n=%u k=%u maximum=%u cycles=%u reads=%llu writes=%llu sat=%u under=%u\n",
            NPU_DMA_LANES,n,k,maximum,unsigned(d.cycle_count),
            (unsigned long long)(reads-r0),(unsigned long long)(writes-w0),expected_status.sat,expected_status.under);
    }
    void raw_compatibility() {
        clear();access(0x4c,true,0);access_unit=8;Fixture f=fixture(3,129,77);load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
        access(0,true,1);await_idle();need(d.done&&!d.error&&d.result_count==f.n*f.groups,"raw-v1 compatibility");
        verify(f);access_unit=4;std::puts("COVER raw_v1_default_contract=pass");
    }
    void bad_config(uint32_t mode,uint32_t sb,uint32_t maximum,uint32_t os,uint32_t expected) {
        Fixture f=fixture(2,129,11);prepare(f,maximum);access(0x4c,true,mode);access(0x50,true,sb);access(0x30,true,os);
        uint64_t before=accepted;access(0,true,1);await_idle();
        need(d.error&&d.error_code==expected&&!d.result_count&&accepted==before,"bad config rejection");
    }
    void fault_cases() {
        for(unsigned kind=0;kind<5;kind++) {
            Fixture f=fixture(9,257,71+kind);prepare(f,0x80000000);
            if(kind==4)put16(scale_base(f),0);
            else {
                fault_armed=true;fault_write=kind==3;
                fault_addr=kind==0?f.ab:kind==1?f.wb+32:kind==2?scale_base(f):f.ob+32;
            }
            access(0,true,1);await_idle();
            const uint32_t codes[]={7,8,11,9,12};
            need(d.error&&!d.done&&d.error_code==codes[kind],"injected fault code "+std::to_string(kind));
            need(d.result_count==(kind==3?8u:0u),"fault must not publish failed output batch");
            if(kind==4)need(access(0x58)==8&&access(0x5c)==0&&access(0x60)==0&&access(0x64)==1,"BadScale diagnostics");
        }
        std::puts("COVER faults=activation,weight,scale,write,BadScale drain=pass ACK_inventory=pass");
    }
    void latency_benchmark() {
        for(unsigned delay:{2u,16u,32u,48u}){
            read_delay=delay;write_delay=delay;response_limit=NPU_WEIGHT_DEPTH;
            std::printf("LATENCY_BENCH depth=%u delay=%u\n",unsigned(NPU_WEIGHT_DEPTH),delay);
            run_fused(128,2048,0x7fffffff,42,0);
        }
    }
    void benchmark() {
        for(auto shape: {std::pair<unsigned,unsigned>{9,4096},{16,2048}}) {
            unsigned n=shape.first,k=shape.second,seed=41+k;
            Fixture f=fixture(n,k,seed);clear();access(0x4c,true,0);access_unit=8;load(f);configure(n,k,f.wb,f.ab,f.ob);
            uint64_t r0=reads,w0=writes;access(0,true,1);await_idle();verify(f);
            std::printf("PERF_BASE_DMA lanes=%u n=%u k=%u cycles=%u reads=%llu writes=%llu\n",NPU_DMA_LANES,n,k,unsigned(d.cycle_count),(unsigned long long)(reads-r0),(unsigned long long)(writes-w0));
            access_unit=4;run_fused(n,k,0x7fffffff,seed,0);
        }
    }
    void tests() {
        need(access(0x4c)==0,"mode reset default");raw_compatibility();
        for(unsigned k:{1u,63u,127u,128u,129u,257u,2048u,3841u,3968u,4096u})
            run_fused(9,k,k==1?1u:0x7fffffffu,41+k,2);
        for(uint32_t maximum:{0u,1u,127u,65536u,0x80000000u})run_fused(17,384,maximum,71,30);
        read_delay=20;write_delay=23;run_fused(11,1024,0x80000000,83,14);read_delay=2;write_delay=2;
        // Coefficient=8192 (FP16 0.5, maximum=127): exact signed .5 ties-even.
        Fixture tie=fixture(9,1,88);prepare(tie,127);
        for(unsigned r=0;r<9;r++)put16(scale_base(tie)+2*r,0x3800);
        access(0,true,1);await_idle();need(d.done&&!d.error,"tie task");
        Numeric ignored;for(unsigned r=0;r<9;r++)need(int32_t(get32(tie.ob+r*4))==rne(__int128(weight(r,0,tie.seed))*activation(0,tie.seed),2,ignored),"RNE14 tie mismatch");
        bad_config(21,0x40000,1,0,10);bad_config(1,0x40001,1,0,12);
        bad_config(1,0x40000,0x80000001,0,12);bad_config(1,0x40000,1,32,4);
        bad_config(1,0x3ffffffe,1,0,5);bad_config(1,fixture(2,129,11).ob,1,0,6);
        fault_cases();raw_compatibility();
        clear();need(access(0x58)==0&&access(0x5c)==0&&access(0x60)==0&&access(0x64)==0,"CLEAR diagnostics");
        std::printf("PASS_NPU_W4_FUSED checks=%u cycles=%llu\n",checks,(unsigned long long)cyc);
    }
};
int main(int argc,char **argv) {
    Verilated::commandArgs(argc,argv);
    try {Tb t;if(argc>1 && std::string(argv[1])=="--latency")t.latency_benchmark();else if(argc>1 && std::string(argv[1])=="--benchmark")t.benchmark();else t.tests();return 0;}catch(const std::exception &e){std::fprintf(stderr,"FAIL_NPU_W4_FUSED %s\n",e.what());return 1;}
}
#endif
