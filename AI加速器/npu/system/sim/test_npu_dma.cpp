#ifndef NPU_DMA_LANES
#define NPU_DMA_LANES 16
#endif
#ifndef NPU_PIPELINE_TEST
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
    Vnpu_dma_top d;
    std::vector<uint8_t> mem=std::vector<uint8_t>(1u<<20,0);
    std::map<uint32_t,uint8_t> high_mem;
    std::deque<Transfer> pending;
    uint64_t cyc=0,reads=0,writes=0,accepted=0,task_committed=0,core_beats=0;
    unsigned checks=0,read_delay=2,write_delay=2;
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
        d.mem_req_ready=!force_stall && ((cyc%5)!=1);
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
                need(bytes && bytes%8==0,"write strobe does not contain complete records");
                task_committed+=bytes/8; write_ack=true;
            }
        }
        if(req_hs) {
            Request q=request();
            need(pending.size()<4,"more than four DDR reads outstanding");
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
    void await_idle(unsigned limit=500000) {
        unsigned guard=0;
        while(d.busy) { step();need(++guard<limit,"DMA task timeout, state="+std::to_string(state())); }
    }
    void run_fixture(const Fixture &f,bool busy_probes=false) {
        clear();load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
        if(f.ws!=f.groups*64) access(40,true,f.ws);
        if(f.os!=((f.groups+3)/4)*32) access(48,true,f.os);
        uint64_t r0=reads,w0=writes,b0=core_beats;expected=f;check_done=true;verified_done=false;
        access(0,true,1);
        if(busy_probes) {
            need(d.busy,"START did not enter iterative validation");
            access(8,true,f.n+1,0,true);access(0,true,1,0,true);access(0,true,2,0,true);
            need(access(8)==f.n&&!d.error,"busy rejection changed configuration/task error");
        }
        await_idle();
        need(d.done&&!d.error&&verified_done,"valid task failed or final DDR results absent at DONE");
        need(access(60)==f.n*f.groups,"wrong committed result count");
        need(reads-r0==(f.k+31)/32+uint64_t(f.n)*f.groups*2,"activation reuse/weight line inventory");
        need(writes-w0==uint64_t(f.n)*((f.groups+3)/4),"four-record write coalescing inventory");
        need(core_beats-b0==uint64_t(f.n)*f.groups*(128/NPU_DMA_LANES),"physical lane accepted-beat count");
        std::printf("PERF lanes=%u n=%u k=%u cycle_count=%u results=%u ddr_reads=%llu ddr_writes=%llu core_beats=%llu\n",
                    NPU_DMA_LANES,
                    f.n,f.k,unsigned(d.cycle_count),unsigned(d.result_count),
                    (unsigned long long)(reads-r0),(unsigned long long)(writes-w0),(unsigned long long)(core_beats-b0));
        check_done=false;
    }
    void run(unsigned n,unsigned k,unsigned seed,bool probes=false) { run_fixture(fixture(n,k,seed),probes); }
    void bad(const std::string &label,unsigned n,unsigned k,uint32_t wb,uint32_t ab,uint32_t ob,
             uint32_t code,std::vector<std::pair<unsigned,uint32_t>> overrides={},bool delayed=false) {
        clear();configure(n,k,wb,ab,ob);for(auto x:overrides) access(x.first,true,x.second);
        uint64_t before=accepted;access(0,true,1);
        if(delayed) need(d.busy&&!d.error,"range/overlap should be deferred: "+label);
        // Admission is acknowledged before range/overlap validation finishes.
        unsigned guard=0;
        while(access(4)&1u) need(++guard<200,"validation timeout: "+label);
        need(d.error&&!d.done&&access(52)==code,"wrong validation error: "+label);
        need(!d.result_count&&accepted==before,"invalid parameters issued DDR requests: "+label);
        access(0,true,1,0,true);clear();
    }
    void validation_cases() {
        bad("N zero",0,128,0x1000,0x8000,0x10000,1);
        bad("N limit",65536,128,0x1000,0x8000,0x10000,1);
        bad("K zero",1,0,0x1000,0x8000,0x10000,2);
        bad("K limit",1,4097,0x1000,0x8000,0x10000,2);
        bad("base alignment",1,128,0x1002,0x8000,0x10000,3);
        for(unsigned hi:{20u,28u,36u}) bad("64-bit high offset",1,128,0x1000,0x8000,0x10000,3,{{hi,1}});
        bad("weight stride",1,128,0x1000,0x8000,0x10000,4,{{40,32}});
        bad("output stride",1,4096,0x1000,0x8000,0x10000,4,{{48,32}});
        bad("activation stride alignment",1,128,0x1000,0x8000,0x10000,4,{{44,33}});
        bad("weight-activation overlap",1,128,0x2000,0x2020,0x10000,6,{},true);
        bad("weight-output overlap",1,128,0x2000,0x8000,0x2020,6,{},true);
        bad("activation-output overlap",1,128,0x1000,0x8000,0x8000,6,{},true);
        bad("high base not truncated",1,128,0xffffffe0u,0x8000,0x10000,5,{},true);
        bad("weight row product wraps 32 bits",3,128,0x1000,0x8000,0x10000,5,{{40,0x80000000u}},true);
        bad("max weight row product",65535,128,0x1000,0x8000,0x10000,5,{{40,0xffffffe0u}},true);
        bad("activation product wraps 32 bits",1,4096,0x1000,0x8000,0x10000,5,{{44,0x80000000u}},true);
        bad("output row product wraps 32 bits",3,128,0x1000,0x8000,0x10000,5,{{48,0x80000000u}},true);
        bad("max output row product",65535,128,0x1000,0x8000,0x10000,5,{{48,0xffffffe0u}},true);
        std::printf("COVER validation=20 async_range_overlap=9 high_word=3 no_ddr_on_reject=pass\n");
    }
    void fault_case(unsigned kind) {
        clear();Fixture f=fixture(kind==2?2:3,kind==2?512:129,120+kind);
        load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
        fault_write=kind==2;fault_addr=kind==0?f.ab:kind==1?f.wb+32:f.ob+f.os;
        fault_armed=true;unsigned injected=faults_fired;
        access(0,true,1);await_idle();
        need(faults_fired==injected+1&&!fault_armed,"requested DDR fault not injected");
        need(d.error&&!d.done&&access(52)==7+kind,"wrong DDR fault status");
        need(d.result_count==(kind==2?4u:0u),"DDR fault committed result count");
        if(kind==2) {
            for(unsigned b=0;b<f.os;++b) need(peek(f.ob+f.os+b)==0xcd,"failed write changed uncommitted row");
        }
        need(pending.empty(),"fault response was not consumed");
        access(0,true,1,0,true);clear();run(f.n,f.k,f.seed+33);
        std::printf("COVER fault=%s code=%u clear_recovery=pass\n",
                    kind==0?"activation_read":kind==1?"weight_read":"output_write",7+kind);
    }
    void reset_cases() {
        for(unsigned target:{10u,2u,6u,9u}) {
            clear();Fixture f=fixture(3,384,200+target);load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
            write_delay=9;access(0,true,1);unsigned guard=0;
            while(state()!=target || (target==6 && d.rootp->npu_dma_top__DOT__beat_idx<(NPU_DMA_LANES==64?1:2))) {
                step();need(++guard<5000,"reset target unreachable");
            }
            need(d.busy,"reset target was not working");
            auto before=mem;auto high_before=high_mem;
            reset();uint64_t requests=accepted;
            for(unsigned i=0;i<24;++i) step();
            need(mem==before&&high_mem==high_before,"DDR changed after coordinated reset");
            need(accepted==requests&&pending.empty(),"post-reset ghost transaction");
            need(access(4)==0&&access(60)==0&&access(56)==0,"post-reset CSR status");
            write_delay=2;run(1,129,250+target);
        }
        std::printf("COVER reset=validation,activation_wait,mid_group,write_wait bridge_reset=coordinated\n");
    }
    void final_write_delay_case() {
        clear();Fixture f=fixture(1,129,303);load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
        write_delay=13;force_stall=true;access(0,true,1);unsigned guard=0;
        while(!d.mem_req_valid) { step();need(++guard<1000,"DDR request never asserted"); }
        for(unsigned i=0;i<9;++i) step(); // force a genuine multi-cycle request hold
        force_stall=false;
        while(state()!=9) { step();need(++guard<5000,"final write wait unreachable"); }
        need(!pending.empty()&&pending.front().write,"missing delayed final write");
        uint64_t ack_at=pending.front().due;
        while(cyc<ack_at) {
            need(d.busy&&!d.done&&!d.result_count,"DONE/count advanced before final write response");
            for(unsigned b=0;b<f.os;++b) need(peek(f.ob+b)==0xcd,"output changed before write acknowledgement");
            step();
        }
        expected=f;check_done=true;verified_done=false;await_idle();
        need(d.done&&verified_done&&d.result_count==2,"delayed final write did not commit");
        check_done=false;write_delay=2;
        std::printf("COVER request_hold=9 final_write_response_delay=13 done_after_ack=pass\n");
    }
    void prefetch_fault_cases() {
        // A failed future row must not cancel the preceding row's write commit.
        clear();Fixture f=fixture(3,128,411);load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
        read_delay=20;fault_write=false;fault_addr=f.wb+f.ws;fault_armed=true;
        access(0,true,1);await_idle();
        need(d.error&&!d.done&&d.error_code==8&&d.result_count==1,
             "future prefetched fault changed preceding committed inventory");
        need(pending.empty(),"future fault left responses undrained");
        need(get32(f.ob)==0,"preceding row tag was not committed");
        for(unsigned b=0;b<f.os;++b) need(peek(f.ob+f.os+b)==0xcd,"faulted row was written");
        clear();
        // Hold an unaccepted read across the older fault response. It must keep
        // VALID/address stable and busy asserted until accepted and drained.
        f=fixture(2,512,412);load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
        fault_addr=f.wb;fault_armed=true;access(0,true,1);unsigned guard=0;
        while(pending.empty() || pending.front().addr!=f.wb) {
            step();need(++guard<5000,"first prefetched read not issued");
        }
        force_stall=true;
        for(unsigned i=0;i<25;++i) step();
        need(d.busy&&!d.error&&d.mem_req_valid&&pending.empty(),
             "fault abandoned held offer or reported before drain");
        force_stall=false;await_idle();
        need(d.error&&d.error_code==8&&!d.result_count&&pending.empty(),"held fault drain failed");
        read_delay=2;clear();run(2,129,413);
        // Errors in every position of a four-record streaming batch must suppress
        // that uncommitted batch, drain reads, and leave the elastic core reusable.
        for(unsigned group=0;group<4;group++){
            clear();f=fixture(1,512,420+group);load(f);configure(f.n,f.k,f.wb,f.ab,f.ob);
            fault_addr=f.wb+group*64;fault_write=false;fault_armed=true;access(0,true,1);await_idle();
            need(d.error&&d.error_code==8&&!d.done&&!d.result_count&&pending.empty(),"streaming group fault commit/drain");
            for(unsigned b=0;b<f.os;b++)need(peek(f.ob+b)==0xcd,"failed batch published partial records");
            clear();run(1,512,430+group);
        }
        std::printf("COVER prefetch_future_fault_commit=pass fault_held_offer_drain=pass\n");
    }
    void test(bool short_run=false) {
        need(access(64,false,0,7)==0x4e444d41,"identity");
        need(access(72)==NPU_DMA_LANES,"physical lane capability CSR");
        access(0x80,false,0,5,true);
        access(8,true,7,0,true,1);need(access(8)==0,"partial-strobe write changed configuration");
        run(1,4096,19); // Prime the synchronous cache before poisoned tail tests.
        for(unsigned k:{1u,15u,16u,17u,31u,32u,33u,63u,64u,65u,127u,128u,129u,255u,256u,257u}) run(2,k,40+k);
        run(3,384,71,true);run(16,2048,77);run(short_run?8:6144,1,91);
        read_delay=20;max_pending=0;uint64_t overlap0=overlap_responses;
        run(8,512,92);
        need(max_pending==4,"prefetch did not reach four outstanding reads");
        need(overlap_responses>overlap0,"DDR responses never overlapped core ISSUE");
        std::printf("COVER max_outstanding_reads=%u response_compute_overlap=%llu\n",max_pending,
                    (unsigned long long)(overlap_responses-overlap0));
        read_delay=2;
        Fixture strided=fixture(3,384,93);strided.ws+=32;strided.os+=32;run_fixture(strided);
        Fixture edge=fixture(1,128,101);edge.wb=0x3fffffc0u;run_fixture(edge);
        edge=fixture(1,384,102);edge.ob=0x3fffffe0u;run_fixture(edge);
        validation_cases();
        for(unsigned kind=0;kind<3;++kind) fault_case(kind);
        reset_cases();final_write_delay_case();prefetch_fault_cases();
        clear();need(pending.empty()&&!d.busy,"regression left an outstanding task");
        std::printf("PASS_NPU_DMA checks=%u cycles=%llu read_faults=4 write_faults=1 mmio_hold=7\n",
                    checks,(unsigned long long)cyc);
    }
};
int main(int argc,char **argv) {
    Verilated::commandArgs(argc,argv);
    try { Tb tb;if(argc>1 && std::string(argv[1])=="--budget"){for(unsigned k:{1024u,2048u,3584u})tb.run(32,k,700+k);return 0;}tb.test(argc>1 && std::string(argv[1])=="--short");return 0; }
    catch(const std::exception &e) { std::fprintf(stderr,"FAIL_NPU_DMA %s\n",e.what());return 1; }
}
#else
#if NPU_DMA_LANES==16
#include "Vgemv_pipeline16.h"
using PipelineDevice=Vgemv_pipeline16;
#else
#include "Vgemv_pipeline_v2.h"
using PipelineDevice=Vgemv_pipeline_v2;
#endif
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>

struct PipelineTb {
    static constexpr unsigned BeatsPerGroup=128/NPU_DMA_LANES;
    struct Beat { uint32_t weights[NPU_DMA_LANES/8]; uint32_t activations[NPU_DMA_LANES/4]; int32_t sum; bool first,last; uint16_t row,group; };
    struct Result { int32_t sum; uint16_t row,group; };
    PipelineDevice d;
    std::deque<Result> expected;
    uint64_t cycles=0,accepted=0,retired=0,input_stalls=0,output_stalls=0;
    unsigned checks=0;int32_t accumulator=0;
    bool held_output=false;
    Result held{};
    void need(bool ok,const std::string &message) { ++checks;if(!ok) throw std::runtime_error(message); }
    static Beat make_beat(unsigned index,unsigned seed) {
        Beat b{};unsigned group=index/BeatsPerGroup,beat=index%BeatsPerGroup;
        b.first=beat==0;b.last=beat==BeatsPerGroup-1;b.row=uint16_t(0x1234+group*17+seed);
        b.group=uint16_t(0xff00-group*7);
        for(unsigned lane=0;lane<NPU_DMA_LANES;++lane) {
            int w=int((index*11+lane*7+seed)%16)-8;
            int a=int((index*31+lane*13+seed)%256)-128;
            if(group%11==0) { w=-8;a=-128; } // +1024 per physical lane
            if(group%11==1) { w=-8;a=127; }  // negative extreme
            b.weights[lane/8]|=uint32_t(w&15)<<(4*(lane%8));
            b.activations[lane/4]|=uint32_t(a&255)<<((lane%4)*8);
            b.sum+=w*a;
        }
        return b;
    }
    void offer(const Beat &b,bool valid,bool ready) {
        d.in_valid=valid;d.out_ready=ready;
#if NPU_DMA_LANES==16
        d.weight_data=uint64_t(b.weights[0])|(uint64_t(b.weights[1])<<32);
#else
        for(unsigned i=0;i<NPU_DMA_LANES/8;++i)d.weight_data[i]=b.weights[i];
#endif
        for(unsigned i=0;i<NPU_DMA_LANES/4;++i)d.activation_data[i]=b.activations[i];
        d.group_first=b.first;d.group_last=b.last;d.row_id=b.row;d.group_id=b.group;
    }
    bool tick(const Beat &b) {
        d.clk=0;d.eval();
        if(held_output) need(d.out_valid&&int32_t(d.partial_sum)==held.sum&&d.out_row_id==held.row&&d.out_group_id==held.group,
                             "pipeline output changed while blocked");
        bool input=d.in_valid&&d.in_ready;
        bool output=d.out_valid&&d.out_ready;
        if(d.in_valid&&!d.in_ready) ++input_stalls;
        if(d.out_valid&&!d.out_ready) ++output_stalls;
        if(input) {
            ++accepted;accumulator=b.first?b.sum:accumulator+b.sum;
            if(b.last) { expected.push_back(Result{accumulator,b.row,b.group});accumulator=0; }
        }
        if(output) {
            need(!expected.empty(),"pipeline duplicate/unexpected output");
            Result r=expected.front();expected.pop_front();++retired;
            need(int32_t(d.partial_sum)==r.sum&&d.out_row_id==r.row&&d.out_group_id==r.group,
                 "pipeline partial or metadata mismatch");
        }
        held_output=d.out_valid&&!d.out_ready;
        if(held_output) held=Result{int32_t(d.partial_sum),uint16_t(d.out_row_id),uint16_t(d.out_group_id)};
        d.clk=1;d.eval();d.clk=0;d.eval();++cycles;
        return input;
    }
    void reset() {
        d.rst_n=0;d.in_valid=0;d.out_ready=0;held_output=false;expected.clear();accumulator=0;
        d.clk=0;d.eval();need(!d.in_ready&&!d.out_valid,"pipeline handshakes survived reset");
        d.clk=1;d.eval();d.clk=0;d.eval();++cycles;
        d.rst_n=1;d.eval();
        Beat idle{};
        for(unsigned i=0;i<8;++i) { offer(idle,false,true);tick(idle);need(!d.out_valid,"pipeline ghost output after reset"); }
    }
    void run(unsigned groups,unsigned seed,bool backpressure) {
        unsigned pos=0,guard=0;bool holding_input=false;
        uint64_t target=retired+groups;
        while(retired<target) {
            bool valid=pos<groups*BeatsPerGroup && (holding_input || !backpressure || guard%7!=3);
            bool ready=!backpressure || (guard>=40 && guard%47>=19);
            Beat b=make_beat(pos,seed);offer(b,valid,ready);
            bool accepted_now=tick(b);holding_input=valid&&!accepted_now;
            if(accepted_now) ++pos;
            need(++guard<groups*100+1000,"pipeline flow timeout");
        }
        need(pos==groups*BeatsPerGroup&&expected.empty(),"pipeline dropped or duplicated group");
        Beat idle{};
        for(unsigned i=0;i<8;++i) { offer(idle,false,true);tick(idle);need(!d.out_valid,"pipeline duplicate tail"); }
    }
    void reset_partial(bool fill_output) {
        unsigned pos=0,guard=0;
        if(!fill_output) {
            while(pos<(BeatsPerGroup>3?3:BeatsPerGroup-1)) { Beat b=make_beat(pos,701);offer(b,true,false);if(tick(b))++pos; }
            need(expected.empty(),"partial reset unexpectedly completed a group");
        } else {
            for(;;) {
                Beat b=make_beat(pos,702);offer(b,true,false);if(tick(b))++pos;
                d.eval();if(d.out_valid&&!d.in_ready) break;
                need(++guard<100,"pipeline did not fill under blocked output");
            }
            need(!expected.empty(),"full reset had no pending result");
        }
        reset();run(5,800+fill_output,true);
    }
    void test() {
        reset();uint64_t start=cycles;run(64,5,false);uint64_t elapsed=cycles-start;
        // Accepted beats + two-stage drain/output retirement + eight idle checks.
        need(elapsed==64*BeatsPerGroup+11,"pipeline full-rate stream inserted an unintended bubble");
        run(96,17,true);need(input_stalls>0&&output_stalls>0,"backpressure did not reach pipeline");
        reset_partial(false);reset_partial(true);
        need(expected.empty()&&!d.out_valid,"pipeline test left output pending");
        std::printf("PASS_GEMV_PIPELINE lanes=%u checks=%u cycles=%llu accepted=%llu retired=%llu input_stalls=%llu output_stalls=%llu fullrate64_cycles=%llu reset_partial=pass reset_full=pass\n",
                    NPU_DMA_LANES,checks,(unsigned long long)cycles,(unsigned long long)accepted,(unsigned long long)retired,
                    (unsigned long long)input_stalls,(unsigned long long)output_stalls,(unsigned long long)elapsed);
    }
};
int main(int argc,char **argv) {
    Verilated::commandArgs(argc,argv);
    try { PipelineTb tb;tb.test();return 0; }
    catch(const std::exception &e) { std::fprintf(stderr,"FAIL_GEMV_PIPELINE16 %s\n",e.what());return 1; }
}
#endif
