/* Extra checks run on the actual RV32 core, production SoC CSR and cache. */
#define main tensor_original_main
#include "tensor_acceptance.c"
#undef main
#define RANGE_START (*(volatile uint32_t *)(uintptr_t)0x10000230u)
#define RANGE_END (*(volatile uint32_t *)(uintptr_t)0x10000234u)
#define RANGE_CTRL (*(volatile uint32_t *)(uintptr_t)0x10000238u)
#define RANGE_CAP (*(volatile uint32_t *)(uintptr_t)0x10000228u)
#ifndef NPU_TEST_SKIP_CACHE
static int range_checks(void) {
 volatile uint32_t *s=(volatile uint32_t *)(uintptr_t)0x10000200u;
 while(!(*s&1u)){}
 if(!(*s&64u)||RANGE_CAP!=0x524e4701u)return 70;
 if(ag_cache_maintain(POLLS))return 71;
 *word(0x60000)=0x12345678;*word(0x6001c)=0x88776655;
 *word(0x62000)=0xaabbccdd;
 uint32_t wb=*(volatile uint32_t *)(uintptr_t)0x10000220u;
 if(ag_cache_maintain_range(0x60004,1,POLLS))return 72;
 if(*(volatile uint32_t *)(uintptr_t)0x10000220u!=wb+1)return 73;
 uint32_t hits=*(volatile uint32_t *)(uintptr_t)0x10000218u;
 if(*word(0x62000)!=0xaabbccdd||*(volatile uint32_t *)(uintptr_t)0x10000218u!=hits+1)return 74;
 /* Invalid raw CSR command must finish without scanning/writing anything. */
 RANGE_START=0x60001;RANGE_END=0x60020;RANGE_CTRL=1;
 if(!(RANGE_CTRL&4u)||(RANGE_CTRL&1u))return 75;
 RANGE_CTRL=2;if(RANGE_CTRL&4u)return 76;
 /* Snapshot A, mutate config to B while scan is busy; only A is maintained. */
 *word(0x60000)=0x31415926;wb=*(volatile uint32_t *)(uintptr_t)0x10000220u;
 RANGE_START=0x60000;RANGE_END=0x60020;RANGE_CTRL=1;
 RANGE_START=0x62000;RANGE_END=0x62020;
 uint32_t left=POLLS;while((RANGE_CTRL&1u)&&left)--left;
 if(!left||(RANGE_CTRL&6u)||*(volatile uint32_t *)(uintptr_t)0x10000220u!=wb+1)return 77;
 /* A short software deadline must retain the hardware maintenance lease. */
 *word(0x60000)=0x27182818;
 int rc=ag_cache_maintain_range(0x60000,32,1);
 if(rc!=-2)return 78;
 left=POLLS;while((*s&8u)&&left)--left;
 if(!left||(*s&0x12u)||ag_cache_maintain_range(0x60000,32,POLLS))return 79;
 ag_cache_range bad[2]={{0x60000,32},{0x3fffffff,2}};
 wb=*(volatile uint32_t *)(uintptr_t)0x10000220u;
 if(ag_cache_maintain_ranges(bad,2,POLLS)!=-1||*(volatile uint32_t *)(uintptr_t)0x10000220u!=wb)return 80;
 ag_cache_range shared[3]={{0x60004,4},{0x6001c,4},{0x60000,16}};
 if(ag_cache_maintain_ranges(shared,3,POLLS))return 81;
 text("PASS_RV32_RANGE_CSR snapshot invalid bounds shared_line neighbor timeout_quiescence\n");return 0;
}
#endif
int main(void){
#ifndef NPU_TEST_SKIP_CACHE
 int rc=range_checks();if(rc)return rc;
#endif
 return tensor_original_main();
}
