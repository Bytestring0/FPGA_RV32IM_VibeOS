#include "platform.h"
#include "npu_runtime.h"
#include <stddef.h>
extern const unsigned char chain_start[],chain_end[];
int main(void){
 while(!(MMIO32(0x10000200u)&1u)){}
 volatile uint32_t*d=(volatile uint32_t*)(uintptr_t)0x80010000u;
 const uint32_t*s=(const uint32_t*)chain_start;size_t words=(size_t)(chain_end-chain_start)/4;
 for(size_t i=0;i<words;i++)d[i]=s[i];
 if(ag_cache_maintain_range(0x10000u,(uint32_t)words*4,1000000u))return 1;
 __asm__ volatile("fence iorw,iorw; fence.i" ::: "memory");
 ((void(*)(void))(uintptr_t)0x80010000u)();return 2;
}
