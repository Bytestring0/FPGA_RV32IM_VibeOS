#include "npu_fixed_math.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
int32_t old_fp16(uint16_t,npu_fixed_status*);
int32_t old_bf16(uint16_t,npu_fixed_status*);
int32_t old_f32(uint32_t,npu_fixed_status*);
static unsigned checks;
static void check(uint32_t bits,unsigned mode){
 npu_fixed_status a={0},b={0};if(checks&1){a=(npu_fixed_status){0,UINT32_MAX,UINT32_MAX-1,UINT32_MAX,UINT32_MAX-1,UINT32_MAX};b=a;}
 int32_t x=mode==0?old_fp16((uint16_t)bits,&a):mode==1?old_bf16((uint16_t)bits,&a):old_f32(bits,&a);
 int32_t y=mode==0?npu_fixed_fp16_bits_to_q16((uint16_t)bits,&b):mode==1?npu_fixed_bf16_bits_to_q16((uint16_t)bits,&b):npu_fixed_f32_bits_to_q16(bits,&b);
 if(x!=y||memcmp(&a,&b,sizeof a)){fprintf(stderr,"DECODE_MISMATCH mode=%u bits=%08x old=%d new=%d flags=%u/%u sat=%u/%u under=%u/%u\n",mode,bits,x,y,a.status,b.status,a.saturations,b.saturations,a.underflows,b.underflows);__builtin_trap();}
 ++checks;
}
int main(void){
 for(unsigned bits=0;bits<65536;bits++){check(bits,0);check(bits,1);}
 const uint32_t fractions[]={0,1,2,3,0x3fffff,0x400000,0x400001,0x7ffffe,0x7fffff};
 for(unsigned sign=0;sign<2;sign++)for(unsigned exp=0;exp<256;exp++)for(unsigned j=0;j<sizeof fractions/sizeof *fractions;j++)check((sign<<31)|(exp<<23)|fractions[j],2);
 uint32_t random=0x3257b9d1;for(unsigned i=0;i<1000000;i++){random^=random<<13;random^=random>>17;random^=random<<5;check(random,2);}
 printf("FIXED_DECODE_EQUAL checks=%u exhaustive_fp16_bf16=1 f32_boundaries_random=1 saturation_counters=1\n",checks);return 0;
}
