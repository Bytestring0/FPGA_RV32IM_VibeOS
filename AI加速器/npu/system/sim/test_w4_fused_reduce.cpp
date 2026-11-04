#include "Vnpu_w4_fused_reduce.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <deque>
struct Test {
 Vnpu_w4_fused_reduce d;
 uint64_t cycles=0,terms=0,rows=0;uint32_t flags=0,sat=0,under=0;int64_t acc=0;
 void tick(){d.clk=0;d.eval();d.clk=1;d.eval();d.clk=0;d.eval();cycles++;}
 void need(bool yes,const char *what){if(!yes)throw std::runtime_error(what);}
 int64_t round(__int128 x,uint64_t den){
  bool neg=x<0;unsigned __int128 a=neg?-x:x,q=a/den,r=a%den;
  if(r>den-r || (r==den-r && (q&1)))q++;
  if(a && !q){flags|=4;under++;}return neg?-int64_t(q):int64_t(q);
 }
 int32_t narrow(int64_t x){if(x>INT32_MAX||x<INT32_MIN){flags|=2;sat++;return x<0?INT32_MIN:INT32_MAX;}return int32_t(x);}
 void clear(){d.clear=1;tick();d.clear=0;flags=sat=under=0;acc=0;tick();}
 void term(int32_t partial,uint16_t bits,uint32_t maximum,bool first,bool last){
  unsigned e=(bits>>10)&31,m=(bits&1023)|(e?1024:0);int sh=e?int(e)-11:-10;
  __int128 numerator=__int128(m)*maximum;uint64_t den=127;
  if(sh>=0)numerator<<=sh;else den<<=-sh;
  int32_t coef=narrow(round(numerator,den));
  if(first)acc=0;acc+=round(__int128(partial)*coef,16384);
  int32_t want=last?narrow(acc):0;
  need(d.in_ready,"unit not ready");d.partial_sum=partial;d.scale_bits=bits;d.maximum=maximum;
  d.first_group=first;d.last_group=last;d.in_valid=1;tick();d.in_valid=0;
  unsigned guard=0;while(!d.term_done){tick();need(++guard<30,"unit timeout");}
  need(!d.invalid_scale,"valid scale rejected");
  if(last){need(int32_t(d.row_result)==want,"unit final Q16 mismatch");rows++;}
  need(d.numeric_flags==flags,"unit flags mismatch");need(d.saturation_count==sat,"unit sat count mismatch");
  need(d.underflow_count==under,"unit underflow count mismatch");terms++;tick();
 }
 void pipeline_stress(){
  clear();struct Expected{bool last;int32_t result;uint32_t flags,sat,under;};std::deque<Expected> queue;
  unsigned sent=0,retired=0;uint32_t random=0x89127831;
  while(retired<4096){
   d.in_valid=sent<4096;
   if(sent<4096){
    random^=random<<13;random^=random>>17;random^=random<<5;
    uint16_t bits=uint16_t(1+random%31743);uint32_t maximum=sent%3?0x80000000:1;
    int32_t partial=int32_t(random);bool first=sent%31==0,last=sent%31==30||sent==4095;
    unsigned e=(bits>>10)&31,m=(bits&1023)|(e?1024:0);int sh=e?int(e)-11:-10;
    __int128 numerator=__int128(m)*maximum;uint64_t den=127;if(sh>=0)numerator<<=sh;else den<<=-sh;
    int32_t coefficient=narrow(round(numerator,den));if(first)acc=0;
    acc+=round(__int128(partial)*coefficient,16384);int32_t result=last?narrow(acc):0;
    queue.push_back({last,result,flags,sat,under});
    need(d.in_ready,"II1 pipeline stalled");d.scale_bits=bits;d.maximum=maximum;d.partial_sum=partial;d.first_group=first;d.last_group=last;sent++;
   }
   tick();
   if(d.term_done){
    need(!queue.empty(),"pipeline duplicate result");auto want=queue.front();queue.pop_front();
    need(bool(d.row_done)==want.last,"pipeline row boundary order");
    if(want.last)need(int32_t(d.row_result)==want.result,"pipeline row Q16 mismatch");
    need(d.numeric_flags==want.flags&&d.saturation_count==want.sat&&d.underflow_count==want.under,"pipeline ordered diagnostics");retired++;
   }
   need(cycles<500000,"pipeline retire timeout");
  }
  d.in_valid=0;tick();need(queue.empty(),"pipeline missing term");
  std::puts("PASS_W4_FUSED_II1 consecutive_groups=4096 row_groups=31 row_boundary_order=pass diagnostics=pass");
 }
 void run(){d.rst=1;d.in_valid=0;d.clear=0;tick();d.rst=0;tick();
  uint32_t random=0x89248137;
  // Every finite positive half exponent, all mantissas, plus full signed32 partials.
  for(unsigned bits=1;bits<0x7c00;bits++){
   random^=random<<13;random^=random>>17;random^=random<<5;
   uint32_t maximum=bits%5==0?0x80000000u:bits%5==1?0:bits%5==2?1:random&0x7fffffffu;
   term(int32_t(random),uint16_t(bits),maximum,(bits-1)%32==0,bits%32==0||bits==0x7bff);
  }
  clear();
  for(int32_t partial:{INT32_MIN,INT32_MAX,-3,-1,1,3}){
   term(partial,0x3800,127,true,true); // exact RNE14 positive/negative half ties
   term(partial,0x7bff,0x80000000,true,true); // coefficient and narrow32 saturation
  }
  for(uint16_t invalid:{uint16_t(0),uint16_t(0x8000),uint16_t(0xbc00),uint16_t(0x7c00),uint16_t(0x7fff)}){
   clear();d.partial_sum=1;d.scale_bits=invalid;d.maximum=1;d.first_group=1;d.last_group=1;d.in_valid=1;tick();d.in_valid=0;
   unsigned guard=0;while(!d.invalid_scale){tick();need(++guard<20,"invalid retire timeout");}
   need(d.invalid_scale&&d.numeric_flags==8&&d.invalid_count==1&&!d.saturation_count&&!d.underflow_count,"invalid scale diagnostic reset");tick();
  }
  pipeline_stress();clear();need(!d.numeric_flags&&!d.invalid_count,"unit CLEAR");
  std::printf("PASS_W4_FUSED_REDUCE terms=%llu rows=%llu all_positive_finite_fp16=31743 signed32_extremes=pass RNE14_ties=pass cycles=%llu\n",
   (unsigned long long)terms,(unsigned long long)rows,(unsigned long long)cycles);
 }
};
int main(int argc,char **argv){Verilated::commandArgs(argc,argv);try{Test t;t.run();return 0;}catch(const std::exception &e){std::fprintf(stderr,"FAIL_W4_FUSED_REDUCE %s\n",e.what());return 1;}}
