/* Production RV32 scalar C W4A8 + exact opcode19 baseline; no host answers.
 * Every operand is read through volatile cached DDR, preventing precomputation.
 * Timer includes output publication for CPU, full production driver for NPU.
 */
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include "npu_fused_matrix.h"
#include "npu_runtime.h"
#define N 16u
#define K 2048u
#define G (K/128u)
#define WB 0x20000u
#define AB 0x30000u
#define SB 0x60000u
#define HO 0x70000u
#define SO 0x71000u
#define MAXIMUM 65536u
#define POLLS 2000000u
static int32_t expected[N],previous[N];
static volatile uint8_t *bytes(uint32_t off){return (volatile uint8_t *)(uintptr_t)(0x80000000u+off);}
static volatile uint32_t *words(uint32_t off){return (volatile uint32_t *)(uintptr_t)(0x80000000u+off);}
static void text(const char*s){while(*s)*(volatile uint32_t*)0x10000000u=(uint8_t)*s++;}
static void number(uint32_t x){char b[10];unsigned n=0;do{b[n++]=(char)('0'+x%10);x/=10;}while(x);while(n)*(volatile uint32_t*)0x10000000u=(uint8_t)b[--n];}
static uint32_t cycle(void){uint32_t x;__asm__ volatile("fence iorw,iorw; csrr %0,cycle":"=r"(x)::"memory");return x;}
static int32_t narrow(int64_t x){return x>INT32_MAX?INT32_MAX:x<INT32_MIN?INT32_MIN:(int32_t)x;}
static uint64_t rne_unsigned(uint64_t x,uint64_t d){uint64_t q=x/d,r=x%d;return q+(r>d-r||(r==d-r&&(q&1)));}
static int64_t rne14(int64_t x){uint64_t mag=x<0?(uint64_t)(-(x+1))+1:(uint64_t)x;uint64_t q=mag>>14,r=mag&16383u;if(r>8192u||(r==8192u&&(q&1)))q++;return x<0?-(int64_t)q:(int64_t)q;}
static int64_t ordered_add(int64_t a,int64_t b){if(b>0&&a>INT64_MAX-b)return INT64_MAX;if(b<0&&a<INT64_MIN-b)return INT64_MIN;return a+b;}
__attribute__((noinline)) static int32_t raw_coefficient(uint16_t bits,uint32_t maximum){
 unsigned e=(bits>>10)&31,m=(bits&1023)|(e?1024:0);int shift=e?(int)e-11:-10;
 uint64_t numerator=(uint64_t)m*maximum,denominator=127;
 if(shift>=0)numerator<<=(unsigned)shift;else denominator<<=(unsigned)-shift;
 return narrow((int64_t)rne_unsigned(numerator,denominator));
}
__attribute__((noinline)) static void software(void){
 volatile uint8_t *w=bytes(WB),*a=bytes(AB);volatile uint16_t *s=(volatile uint16_t*)bytes(SB);
 for(unsigned row=0;row<N;row++){
  int64_t total=0;
  for(unsigned group=0;group<G;group++){
   int32_t partial=0;
   for(unsigned j=0;j<128;j+=2){
    unsigned col=group*128+j;uint32_t packed=w[row*(K/2)+col/2];
    int32_t lo=(int32_t)(packed&15);if(lo>=8)lo-=16;
    int32_t hi=(int32_t)(packed>>4);if(hi>=8)hi-=16;
    int32_t a0=(int8_t)a[col],a1=(int8_t)a[col+1];partial+=lo*a0+hi*a1;
   }
   int32_t coefficient=raw_coefficient(s[row*G+group],MAXIMUM);
   total=ordered_add(total,rne14((int64_t)partial*coefficient));
  }
  expected[row]=narrow(total);words(SO)[row]=(uint32_t)expected[row];
 }
}
static void prepare(void){
 for(unsigned r=0;r<N;r++)for(unsigned c=0;c<K;c+=8){uint32_t v=0;for(unsigned q=0;q<8;q++)v|=((r*11+(c+q)*7+31)&15)<<(q*4);words(WB)[r*(K/8)+c/8]=v;}
 for(unsigned c=0;c<K;c+=4){uint32_t v=0;for(unsigned q=0;q<4;q++)v|=((uint32_t)((int)(((c+q)*13+31)%255)-127)&255)<<(q*8);words(AB)[c/4]=v;}
 static const uint16_t scales[8]={0x0401,0x1800,0x2800,0x3400,0x3800,0x3c00,0x4000,0x4800};
 for(unsigned r=0;r<N;r++)for(unsigned g=0;g<G;g+=2)words(SB)[(r*G+g)/2]=(uint32_t)scales[(r*3+g)&7]|((uint32_t)scales[(r*3+g+1)&7]<<16);
 for(unsigned r=0;r<N;r++){words(HO)[r]=0xa5a5a5a5u;words(SO)[r]=0xa5a5a5a5u;}
}
int main(void){
 volatile uint32_t *ddr=(volatile uint32_t*)0x10000200u;unsigned polls=POLLS;while(polls--&&!(*ddr&1)){}
 if(!(*ddr&1)||(*ddr&0x12)||!(*ddr&0x20))return 1;
 npu_raw_region regions[3]={{WB,N*K/2,NPU_RAW_WEIGHT},{AB,K,NPU_RAW_ACTIVATION},{0x40000,N*G*8,NPU_RAW_PARTIAL}};
 npu_raw_context raw={0};raw.config.mode=NPU_RAW_HARDWARE;raw.config.device=npu_dma_mmio();raw.config.regions=regions;raw.config.region_count=3;
 npu_fused_matrix_job job={0};job.raw.rows=N;job.raw.cols=K;job.raw.weight_offset=WB;job.raw.activation_offset=AB;job.raw.partial_offset=0x40000;
 job.scale_offset=SB;job.output_offset=HO;job.maximum=MAXIMUM;job.scale_region_offset=SB;job.scale_region_bytes=N*G*2;job.output_region_offset=HO;job.output_region_bytes=N*4;
 prepare();
 for(unsigned pass=0;pass<2;pass++){
  /* Both paths start clean, invalid D/I caches; this equal-state setup is outside
   * timers. Actual NPU driver clean/invalidate, program, poll, acquire is timed. */
  if(ag_cache_maintain(POLLS))return 2;
  uint32_t start=cycle();software();
  if(ag_cache_maintain_range(SO,N*4,POLLS))return 3;
  uint32_t sw=cycle()-start;
  for(unsigned r=0;r<N;r++){if((int32_t)words(SO)[r]!=expected[r])return 4;if(pass&&expected[r]!=previous[r])return 5;previous[r]=expected[r];}
  if(ag_cache_maintain(POLLS))return 6;
  npu_fused_matrix_stats stats={0};npu_raw_budgets budget={POLLS,POLLS};start=cycle();
  int rc=npu_fused_matrix_execute(&raw,&job,budget,&stats);uint32_t hw=cycle()-start;
  if(rc||stats.error||stats.result_count!=N||raw.active||raw.cache_unknown)return 7;
  for(unsigned r=0;r<N;r++){
   int32_t got=(int32_t)words(HO)[r];if(got!=expected[r]){text("Q16_MISMATCH row=");number(r);text(" got=");number((uint32_t)got);text(" expected=");number((uint32_t)expected[r]);text("\n");return 8;}
  }
  text("RV32_FUSED_BENCH {\"pass\":");number(pass);text(",\"n\":");number(N);text(",\"k\":");number(K);text(",\"cpu_cycles\":");number(sw);text(",\"hardware_job_cpu_cycles\":");number(hw);text(",\"hardware_engine_cycles\":");number(stats.cycles);text(",\"rows_compared\":");number(N);text(",\"q16\":[");for(unsigned r=0;r<N;r++){if(r)text(",");number((uint32_t)expected[r]);}text("]}\n");
 }
 text("PASS_RV32_FUSED_BENCH\n");return 0;
}
