#include "Vddr_queue4_tb.h"
#include "verilated.h"
#include <array>
#include <deque>
#include <map>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <algorithm>
using Line=std::array<uint32_t,8>;
static void need(bool b,const char*s){if(!b)throw std::runtime_error(s);}
struct Command {uint32_t addr;bool write=false,bad=false;Line data{};uint32_t strobe=0xffffffff;};
struct Expected {Line data{};bool error=false;unsigned sequence=0;};
struct Read {uint32_t addr;uint64_t due;};
struct Bench {
 Vddr_queue4_tb d;
 unsigned cpu_half,ui_half;uint64_t time=0,cpu_edges=0,ui_edges=0;
 uint32_t rng=0x194bad31;unsigned accepted=0,returned=0,reads=0,writes=0,peak_reads=0,peak_credit=0,owner_changes=0,errors=0;
 unsigned starvation[3]={0,0,0},source_accepts[3]={0,0,0},max_starvation=0;
 std::deque<Command> requests[3];std::deque<Expected> expected[3];std::deque<unsigned> owners;
 std::deque<Read> readpipe;
 std::map<uint32_t,Line> memory,reference;
 bool wp=false,silent=false,ui_running=true,force_error=false,random_stall=true,hold_responses=false,expect_fault=false;
 bool bad_rid=false,bad_rlast=false,bad_wid=false,bad_wlast=false;
 unsigned latency=20;uint32_t wa=0;int lastowner=-1;
 bool held_rsp[3]={false,false,false},held_ar=false,held_aw=false;Expected held[3];uint32_t arheld=0,awheld=0;Line wheld{};uint32_t wsheld=0;
 Bench(unsigned a,unsigned b):cpu_half(a),ui_half(b){d.cpu_clk=0;d.ui_clk=0;d.reset_n=0;d.init_done=0;}
 static Line initial(uint32_t addr){Line l;for(unsigned i=0;i<8;i++)l[i]=0x126934afu^(addr*7919u+i*0x113579u);return l;}
 Line line(std::map<uint32_t,Line>&m,uint32_t a){auto i=m.find(a);return i==m.end()?initial(a):i->second;}
 void apply(std::map<uint32_t,Line>&m,uint32_t a,const Line&v,uint32_t strobe){Line old=line(m,a);for(unsigned b=0;b<32;b++)if(strobe&(1u<<b)){unsigned sh=b%4*8;old[b/4]=(old[b/4]&~(255u<<sh))|(v[b/4]&(255u<<sh));}m[a]=old;}
 void reset(bool calibrate=true){
  for(unsigned s=0;s<3;s++){starvation[s]=0;requests[s].clear();expected[s].clear();held_rsp[s]=false;}owners.clear();readpipe.clear();
  wp=false;silent=false;ui_running=true;force_error=false;hold_responses=false;expect_fault=false;
  bad_rid=bad_rlast=bad_wid=bad_wlast=false;held_rsp[0]=held_rsp[1]=held_ar=held_aw=false;
  d.reset_n=0;d.init_done=0;run(80*std::max(cpu_half,ui_half));d.reset_n=1;
  run(30*std::max(cpu_half,ui_half));if(calibrate){d.init_done=1;run(30*std::max(cpu_half,ui_half));need(d.ready_cpu,"ready after common reset");}
 }
 void add(unsigned source,uint32_t addr,bool wr=false,uint32_t mask=0xffffffff){Command c;c.addr=addr;c.write=wr;c.bad=addr%32;c.strobe=mask;for(unsigned i=0;i<8;i++)c.data[i]=(addr<<8)^0x87654321u^(i*0x19431u);requests[source].push_back(c);}
 void step(){
  rng=(rng>>1)^((0u-(rng&1u))&0xd0000001u);
  bool cr=(time+1)%(2*cpu_half)==cpu_half,cf=(time+1)%(2*cpu_half)==0;
  bool ur=ui_running&&(time+1)%(2*ui_half)==ui_half,uf=ui_running&&(time+1)%(2*ui_half)==0;
  for(unsigned s=0;s<3;s++){
   bool v=!requests[s].empty()&&d.reset_n;
   auto c=v?requests[s].front():Command{};
   if(s==0){d.a_req_valid=v;d.a_req_addr=c.addr;d.a_req_write=c.write;d.a_req_wstrb=c.strobe;for(unsigned i=0;i<8;i++)d.a_req_wdata[i]=c.data[i];d.a_rsp_ready=!hold_responses&&(!random_stall||(rng&3u)!=0);}
   else if(s==1){d.b_req_valid=v;d.b_req_addr=c.addr;d.b_req_write=c.write;d.b_req_wstrb=c.strobe;for(unsigned i=0;i<8;i++)d.b_req_wdata[i]=c.data[i];d.b_rsp_ready=!hold_responses&&(!random_stall||(rng&12u)!=0);}
   else {d.c_req_valid=v;d.c_req_addr=c.addr;d.c_req_write=c.write;d.c_req_wstrb=c.strobe;for(unsigned i=0;i<8;i++)d.c_req_wdata[i]=c.data[i];d.c_rsp_ready=!hold_responses&&(!random_stall||(rng&48u)!=0);}
  }
  d.axi_arready=d.reset_n&&!silent&&(!random_stall||(rng&3u)!=0)&&readpipe.size()<4;
  d.axi_awready=d.reset_n&&!silent&&!wp&&(!random_stall||(rng&12u)!=0);
  d.axi_wready=d.reset_n&&!silent&&(wp||d.axi_awvalid)&&(!random_stall||(rng&48u)!=0);
  bool fast_read=latency==0&&readpipe.empty()&&d.axi_arvalid&&d.axi_arready;
  d.axi_rvalid=d.reset_n&&!silent&&((!readpipe.empty()&&readpipe.front().due<=ui_edges)||fast_read);
  d.axi_rid=bad_rid?7:0;d.axi_rlast=!bad_rlast;d.axi_wusero_id=bad_wid?3:0;d.axi_wusero_last=!bad_wlast;
  Line rd=readpipe.empty()?line(memory,d.axi_araddr<<2):line(memory,readpipe.front().addr);
  for(unsigned i=0;i<8;i++)d.axi_rdata[i]=rd[i];
  d.eval();
  bool ar=ur&&d.axi_arvalid&&d.axi_arready,aw=ur&&d.axi_awvalid&&d.axi_awready;
  bool wc=ur&&d.axi_wready&&(wp||aw),rc=ur&&d.axi_rvalid;
  uint32_t ara=d.axi_araddr<<2,awa=d.axi_awaddr<<2,wm=d.axi_wstrb;Line wd;for(unsigned i=0;i<8;i++)wd[i]=d.axi_wdata[i];
  if(cr&&d.reset_n){
   cpu_edges++;
   for(unsigned s=0;s<3;s++){
    bool qready=s==0?d.a_req_ready:s==1?d.b_req_ready:d.c_req_ready;
    bool valid=s==0?d.a_rsp_valid:s==1?d.b_rsp_valid:d.c_rsp_valid;
    bool ready=s==0?d.a_rsp_ready:s==1?d.b_rsp_ready:d.c_rsp_ready;
    bool error=s==0?d.a_rsp_error:s==1?d.b_rsp_error:d.c_rsp_error;
    Line data;for(unsigned i=0;i<8;i++)data[i]=s==0?d.a_rsp_data[i]:s==1?d.b_rsp_data[i]:d.c_rsp_data[i];
    if(held_rsp[s])need(valid&&data==held[s].data&&error==held[s].error,"response changed under backpressure");
    held_rsp[s]=valid&&!ready;if(held_rsp[s]){held[s].data=data;held[s].error=error;}
    if(valid&&ready){
     need(!expected[s].empty(),"unsolicited/wrong-owner response");need(!owners.empty()&&owners.front()==s,"response owner FIFO mismatch");owners.pop_front();
     auto e=expected[s].front();expected[s].pop_front();
     if(error!=e.error)std::fprintf(stderr,"MISMATCH time=%llu seq=%u source=%u goterr=%u experr=%u fault=%u ready=%u accepted=%u returned=%u reads=%u writes=%u pending=%zu\n",(unsigned long long)time,e.sequence,s,error,e.error,d.fault_cpu,d.ready_cpu,accepted,returned,reads,writes,owners.size());
     need(error==e.error,"response error ordering mismatch");if(!error)need(data==e.data,"transport data mismatch");else errors++;
     returned++;
    }
    if(!requests[s].empty()&&qready){
     auto c=requests[s].front();requests[s].pop_front();Expected e;
     e.error=c.bad||!d.ready_cpu||force_error;e.sequence=accepted;
     if(!e.error){if(c.write)apply(reference,c.addr,c.data,c.strobe);else e.data=line(reference,c.addr);}
     expected[s].push_back(e);owners.push_back(s);accepted++;
     source_accepts[s]++;
     for(unsigned k=0;k<3;k++){if(k==s||requests[k].empty())starvation[k]=0;else {starvation[k]++;max_starvation=std::max(max_starvation,starvation[k]);need(starvation[k]<=4,"two-level RR starvation bound");}}
     if(lastowner>=0&&lastowner!=int(s))owner_changes++;lastowner=int(s);
     peak_credit=std::max(peak_credit,unsigned(owners.size()));need(owners.size()<=4,"accepted credit exceeds four");
    }
   }
  }
  if(ur&&d.reset_n){
   ui_edges++;
   if(!expect_fault){
    if(held_ar)need(d.axi_arvalid&&d.axi_araddr==arheld,"AR changed under backpressure");
    if(held_aw)need(d.axi_awvalid&&d.axi_awaddr==awheld&&wd==wheld&&wm==wsheld,"AW/payload changed under backpressure");
   }
   held_ar=d.axi_arvalid&&!d.axi_arready;arheld=d.axi_araddr;
   held_aw=d.axi_awvalid&&!d.axi_awready;awheld=d.axi_awaddr;wheld=wd;wsheld=wm;
   if(wp)need(wd==wheld_pending&&wm==wm_pending,"write payload changed after AW");
  }
  if(cr)d.cpu_clk=1;if(cf)d.cpu_clk=0;if(ur)d.ui_clk=1;if(uf)d.ui_clk=0;d.eval();
  if(ur&&d.reset_n){
   bool immediate=rc&&readpipe.empty();
   if(rc&&!readpipe.empty())readpipe.pop_front();
   if(ar){need(!(ara&31),"unaligned AR reached controller");if(!immediate)readpipe.push_back({ara,ui_edges+latency});reads++;peak_reads=std::max(peak_reads,unsigned(readpipe.size()));}
   if(aw){need(!(awa&31),"unaligned AW reached controller");wa=awa;wp=true;wheld_pending=wd;wm_pending=wm;}
   if(wc){apply(memory,wa,wd,wm);wp=false;writes++;}
  }
  time++;Verilated::timeInc(1);
 }
 Line wheld_pending{};uint32_t wm_pending=0;
 void run(uint64_t count){for(uint64_t i=0;i<count;i++)step();}
 void drain(uint64_t limit=300000){while(limit--&&(!requests[0].empty()||!requests[1].empty()||!requests[2].empty()||!owners.empty()))step();need(requests[0].empty()&&requests[1].empty()&&requests[2].empty()&&owners.empty(),"drain watchdog");}
 void errors_for_pending(){force_error=true;for(auto &q:expected)for(auto &e:q){e.error=true;e.data={};}expect_fault=true;}
 void wait_reads(unsigned n){unsigned limit=20000;while(limit--&&readpipe.size()<n)step();need(readpipe.size()>=n,"did not pipeline requested AR count");}
 void traffic(){
  reset(false);add(0,0x100);add(1,0x120);drain();need(!d.fault_cpu,"precalibration error must not fault");d.init_done=1;run(100);need(d.ready_cpu,"calibration ready");
  latency=32;random_stall=true;
  for(unsigned i=0;i<80;i++){add(i%3,0x1000+i*32);if(i%9==0)add((i+1)%3,0x1804+i*32);}
  drain();need(!d.fault_cpu,"normal reads/unaligned fault");need(peak_reads>=3,"multiple AR not exercised");
  for(unsigned i=0;i<40;i++)add(i%3,0x4000+i*32,true,i%2?0x55555555u:0xffffffffu);drain();
  for(unsigned i=0;i<40;i++)add((i+1)%3,0x4000+i*32);drain();
  hold_responses=true;for(unsigned i=0;i<4;i++)add(i%3,0x6000+i*32);run(800*std::max(cpu_half,ui_half));
  need(owners.size()==4,"return credits not reserved");need(!d.fault_cpu,"consumer backpressure became fault");hold_responses=false;drain();
  latency=0;random_stall=false;for(unsigned i=0;i<16;i++)add(i%3,0x7000+i*32);drain();
  for(unsigned i=0;i<6;i++)add(i%3,0x7800+i*32,true);drain();random_stall=true;latency=20;
 }
 void fault_cases(){
  reset();latency=100;for(unsigned i=0;i<4;i++)add(i%3,0x8000+i*32);wait_reads(4);errors_for_pending();bad_rid=true;drain();run(100);need(d.fault_cpu,"bad RID fault not latched");
  bad_rid=false;run(1000);need(!d.a_rsp_valid&&!d.b_rsp_valid&&!d.c_rsp_valid,"late R created duplicate response");need(!d.a_req_ready&&!d.b_req_ready,"fault accepted new task");
  reset();for(unsigned i=0;i<4;i++)add(i%3,0x9000+i*32);wait_reads(3);errors_for_pending();bad_rlast=true;drain();need(d.fault_cpu,"bad RLAST fault");
  reset();errors_for_pending();bad_wlast=true;add(0,0xa000,true);drain();run(100);need(d.fault_cpu,"bad WLAST fault");
  reset();errors_for_pending();bad_wid=true;add(1,0xa020,true);drain();run(100);need(d.fault_cpu,"bad WID fault");
  reset();latency=100;for(unsigned i=0;i<4;i++)add(i%3,0xb000+i*32);wait_reads(4);errors_for_pending();d.init_done=0;drain();d.init_done=1;run(1200);need(d.fault_cpu,"calibration loss fault not sticky");
  reset();latency=10000;for(unsigned i=0;i<4;i++)add(i%3,0xc000+i*32);wait_reads(4);errors_for_pending();drain();need(d.fault_cpu,"UI timeout fault");latency=1;run(30000);need(!d.a_rsp_valid&&!d.b_rsp_valid&&!d.c_rsp_valid,"late timeout return not quarantined");
  reset();ui_running=false;errors_for_pending();for(unsigned i=0;i<4;i++)add(i%3,0xd000+i*32);drain();need(d.fault_cpu,"stopped UI CPU timeout");ui_running=true;run(1000);need(!d.a_rsp_valid&&!d.b_rsp_valid&&!d.c_rsp_valid,"UI restart ghost response");
  reset();latency=100;for(unsigned i=0;i<4;i++)add(i%3,0xe000+i*32);wait_reads(4);reset();run(1000);need(!d.a_rsp_valid&&!d.b_rsp_valid&&!d.c_rsp_valid,"common reset ghost response");add(0,0xf000);add(1,0xf020);drain();need(!d.fault_cpu,"common reset did not recover");
 }
};
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);unsigned a=argc>1?std::stoul(argv[1]):5,b=argc>2?std::stoul(argv[2]):3;try{Bench t(a,b);t.traffic();t.fault_cases();printf("DDR_QUEUE4_SOURCES cpu=%u raw=%u tensor=%u max_other_accepts=%u reset_aborts=4\n",t.source_accepts[0],t.source_accepts[1],t.source_accepts[2],t.max_starvation);printf("DDR_QUEUE4_PASS {\"cpu_half\":%u,\"ui_half\":%u,\"accepted\":%u,\"returned\":%u,\"reads\":%u,\"writes\":%u,\"peak_reads\":%u,\"peak_credit\":%u,\"owner_changes\":%u,\"errors\":%u}\n",a,b,t.accepted,t.returned,t.reads,t.writes,t.peak_reads,t.peak_credit,t.owner_changes,t.errors);return 0;}catch(const std::exception&e){fprintf(stderr,"DDR_QUEUE4_FAIL ratio=%u:%u %s\n",a,b,e.what());return 1;}}
