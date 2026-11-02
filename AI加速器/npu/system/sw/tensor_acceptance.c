#include <stdint.h>
#include <stddef.h>
#include "npu_tensor_driver.h"
#include "npu_runtime.h"
#include "npu_dma.h"

#define BASE 0x10000u
#define WORDS 512u
#define POLLS 200000u
#define POISON 0xa5a5a5a5u
struct patch { uint32_t word,value; };
struct fixture {
    const char *name;
    npu_tensor_job job;
    const struct patch *initial,*expected;
    uint32_t initial_count,expected_count,kind;
    npu_tensor_stats stats;
};
#include "tensor_system_fixtures.h"
static uint32_t expected[WORDS],checked;
static npu_tensor_context context;
static npu_tensor_cache_fn normal_publish;
static volatile uint32_t *word(uint32_t off) {return (volatile uint32_t *)(uintptr_t)(0x80000000u+off);}
static void text(const char *s) {while(*s)*(volatile uint32_t *)0x10000000u=(uint8_t)*s++;}
static void number(uint32_t x) {char b[10];unsigned n=0;do{b[n++]=(char)('0'+x%10);x/=10;}while(x);while(n)*(volatile uint32_t *)0x10000000u=(uint8_t)b[--n];}
static int weight(unsigned row,unsigned col) {return (int)((row*11+col*7+31)&15)-8;}
static int activation(unsigned col) {return (int)((col*13+31)%255)-127;}
static void matrix_prepare(unsigned n,unsigned k,uint32_t out) {
    unsigned padded=(k+127)&~127u,groups=padded/128,stride=(groups*8+31)&~31u;
    for(unsigned r=0;r<n;r++)for(unsigned c=0;c<padded;c+=8){uint32_t v=0;for(unsigned j=0;j<8;j++)v|=(uint32_t)(weight(r,c+j)&15)<<(4*j);*word(0x20000+r*padded/2+c/2)=v;}
    for(unsigned c=0;c<padded;c+=4){uint32_t v=0;for(unsigned j=0;j<4;j++)v|=(uint32_t)(activation(c+j)&255)<<(8*j);*word(0x30000+c)=v;}
    for(unsigned i=0;i<n*stride;i+=4)*word(out+i)=POISON;
}
static void matrix_start(unsigned n,unsigned k,uint32_t out) {
    volatile npu_dma_regs *r=npu_dma_mmio();
    npu_dma_clear(r);npu_dma_program(r,n,k,0x20000,0x30000,out,0,0,0);npu_dma_start(r);
}
static int matrix_check(unsigned n,unsigned k,uint32_t out) {
    uint32_t error;unsigned groups=(k+127)/128,stride=(groups*8+31)&~31u;
    if(npu_dma_wait(npu_dma_mmio(),POLLS,&error)||ag_dma_acquire(POLLS))return 31;
    if(npu_dma_mmio()->result_count!=n*groups)return 32;
    for(unsigned r=0;r<n;r++)for(unsigned g=0;g<groups;g++){
        int32_t sum=0;for(unsigned c=g*128;c<(g+1)*128&&c<k;c++)sum+=weight(r,c)*activation(c);
        if(*word(out+r*stride+g*8)!=(r|(g<<16))||(int32_t)*word(out+r*stride+g*8+4)!=sum)return 33;
        checked+=2;
    }
    for(unsigned r=0;r<n;r++)for(unsigned b=groups*8;b<stride;b+=4)if(*word(out+r*stride+b)!=POISON)return 34;
    text("MATRIX_V1 {\"n\":");number(n);text(",\"k\":");number(k);text(",\"cycles\":");number(npu_dma_mmio()->cycle_count);text("}\n");
    return 0;
}
static int concurrent_publish(void *u,const npu_tensor_layout *l,uint32_t polls) {
    int rc=normal_publish(u,l,polls);if(rc)return rc;
    /* This independent matrix job owns separate buffers. A tensor publish
     * covers only its declared spans, so publish the matrix lease explicitly. */
    ag_cache_range matrix_ranges[3]={{0x20000,16u*1024u},{0x30000,2048u},{0x40000,16u*128u}};
    rc=ag_cache_maintain_ranges(matrix_ranges,3,polls);if(rc)return rc;
    matrix_start(16,2048,0x40000);
    for(unsigned i=0;i<4;i++)*word(0x50000+i*64)=i^0x12345678u;
    return 0;
}
static int compare_stats(const npu_tensor_stats *a,const npu_tensor_stats *b) {
    const uint32_t *x=(const uint32_t *)a,*y=(const uint32_t *)b;
    for(unsigned i=0;i<sizeof *a/4;i++)if(x[i]!=y[i]){text("STATS_MISMATCH field=");number(i);text(" got=");number(x[i]);text(" expected=");number(y[i]);text("\n");return 1;}
    return 0;
}
static int run_fixture(const struct fixture *f,unsigned index) {
    if(f->kind!=1)for(unsigned i=0;i<WORDS;i++){*word(BASE+4*i)=POISON;expected[i]=POISON;}
    for(unsigned i=0;i<f->initial_count;i++){
        const struct patch *p=f->initial+i;expected[p->word]=p->value;
        /* Pipeline raw records come from matrix hardware, never oracle writes. */
        if(f->kind!=2||p->word>=16)*word(BASE+4*p->word)=p->value;
    }
    if(f->kind==2){matrix_prepare(2,129,BASE);if(ag_dma_publish(POLLS))return 35;matrix_start(2,129,BASE);int rc=matrix_check(2,129,BASE);if(rc)return rc;}
    if(f->kind==3){matrix_prepare(16,2048,0x40000);context.config.publish=concurrent_publish;}
    npu_tensor_stats actual={0};
    int rc=npu_tensor_execute(&context,&f->job,&actual);
    context.config.publish=normal_publish;
    if(rc){text("EXECUTE_FAILURE fixture=");number(index);text(" rc=");number((uint32_t)-rc);text(" error=");number(context.last_device_error);text("\n");return 11;}
    if(compare_stats(&actual,&f->stats))return 12;
    for(unsigned i=0;i<f->expected_count;i++){const struct patch *p=f->expected+i;expected[p->word]=p->value;}
    for(unsigned i=0;i<WORDS;i++){
        uint32_t want=expected[i];
#ifdef NEGATIVE_EXPECTATION
        if(index==0&&i==192)want^=1u;
#endif
        uint32_t got=*word(BASE+4*i);
        if(got!=want){text("DDR_MISMATCH fixture=");number(index);text(" word=");number(i);text(" got=");number(got);text(" expected=");number(want);text("\n");return 10;}
        checked++;
    }
    if(f->kind==3){rc=matrix_check(16,2048,0x40000);if(rc)return rc;for(unsigned i=0;i<4;i++)if(*word(0x50000+i*64)!=(i^0x12345678u))return 36;}
    text("TENSOR_SYSTEM_CASE {\"index\":");number(index);text(",\"opcode\":");number(f->job.opcode);text(",\"name\":\"");text(f->name);text("\",\"cycles\":");number(context.last_cycles);text("}\n");
    return 0;
}
int main(void) {
    volatile uint32_t *status=(volatile uint32_t *)0x10000200u;unsigned polls=POLLS;
    while(polls--&&!(*status&1u)){}
    if(!(*status&1u)||(*status&0x12u)||!(*status&0x20u))return 1;
    if(npu_dma_mmio()->id!=NPU_DMA_ID_VALUE)return 2;
    npu_tensor_region region={BASE,WORDS*4,3};
    if(npu_tensor_ag_init(&context,&region,1,POLLS,POLLS))return 3;
    normal_publish=context.config.publish;
    for(unsigned i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);i++){int rc=run_fixture(fixtures+i,i);if(rc)return rc;}
    matrix_prepare(2,129,0x40000);if(ag_dma_publish(POLLS))return 37;matrix_start(2,129,0x40000);int rc=matrix_check(2,129,0x40000);if(rc)return rc;
    if(context.active||context.cache_unknown||context.jobs_started!=context.jobs_completed||context.jobs_completed!=sizeof(fixtures)/sizeof(fixtures[0]))return 38;
    text("TENSOR_SYSTEM_SUMMARY {\"jobs\":");number((uint32_t)context.jobs_completed);text(",\"checked_words\":");number(checked);text(",\"opcodes\":");number(TENSOR_SYSTEM_OPCODE_COUNT);text(",\"matrix_jobs\":3,\"reference_on_target\":false}\n");
    text("PASS_TENSOR_CPU_CACHE_DDR_V2\n");return 0;
}
