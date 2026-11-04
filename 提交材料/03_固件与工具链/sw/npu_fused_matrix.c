#include "npu_fused_matrix.h"
#include "npu_runtime.h"
#include <string.h>
#if defined(__riscv) || defined(NPU_FUSED_MATRIX_TEST_MMIO)
static npu_raw_context *owner;
static ag_cache_range lease[4];
static void barrier(void){
#if defined(__riscv)
 __asm__ volatile("fence iorw,iorw":::"memory");
#else
 __asm__ volatile("":::"memory");
#endif
}
#ifdef NPU_FUSED_MATRIX_TEST_MMIO
extern uint32_t npu_fused_test_read32(volatile uint32_t *);
extern void npu_fused_test_write32(volatile uint32_t *,uint32_t);
#define rd(p) npu_fused_test_read32(p)
#define wr(p,v) npu_fused_test_write32(p,v)
#else
#define rd(p) (*(p))
#define wr(p,v) (*(p)=(v))
#endif
static volatile uint32_t *extra(volatile npu_dma_regs *r,uint32_t off){return (volatile uint32_t *)((uintptr_t)r+off);}
static int unknown(npu_raw_context *c){c->active=1;c->cache_unknown=1;c->last_status=NPU_RAW_UNKNOWN;return NPU_RAW_UNKNOWN;}
static int owned(uint32_t base,uint32_t bytes,uint32_t offset,uint32_t span){
 return bytes&&base<NPU_RAW_DDR_LIMIT&&bytes<=NPU_RAW_DDR_LIMIT-base&&offset>=base&&offset-base<=bytes&&span<=bytes-(offset-base);
}
static int overlap(ag_cache_range a,ag_cache_range b){return a.offset<(uint64_t)b.offset+b.bytes&&b.offset<(uint64_t)a.offset+a.bytes;}
#endif
int npu_fused_matrix_execute(npu_raw_context *c,const npu_fused_matrix_job *in,npu_raw_budgets budget,npu_fused_matrix_stats *stats){
#if !defined(__riscv) && !defined(NPU_FUSED_MATRIX_TEST_MMIO)
 (void)c;(void)in;(void)budget;(void)stats;return NPU_RAW_HOST_UNSUPPORTED;
#else
 npu_fused_matrix_job j;npu_raw_layout l;uint32_t status=0;int terminal=0;
 npu_fused_matrix_stats result={0};
 if(!c||!in||!stats||!budget.device_polls||!budget.cache_polls)return NPU_RAW_ARGUMENT;
 if(c->active||c->cache_unknown||owner)return NPU_RAW_UNKNOWN;
 if(c->config.mode!=NPU_RAW_HARDWARE||!c->config.device)return NPU_RAW_ARGUMENT;
 j=*in;
 if(npu_raw_query(&j.raw,c->config.regions,c->config.region_count,&l))return NPU_RAW_ARGUMENT;
 if((j.scale_offset&1u)||(j.output_offset&31u)||j.maximum>0x80000000u||
    !owned(j.scale_region_offset,j.scale_region_bytes,j.scale_offset,j.raw.rows*l.groups*2u)||
    !owned(j.output_region_offset,j.output_region_bytes,j.output_offset,(j.raw.rows*4u+31u)&~31u))return NPU_RAW_REGION;
 ag_cache_range spans[4]={{j.raw.weight_offset,l.weight_span},{j.raw.activation_offset,l.activation_span},
  {j.scale_offset,j.raw.rows*l.groups*2u},{j.output_offset,j.raw.rows*4u}};
 for(unsigned q=0;q<3;q++){
  /* Cache ownership is a full line even for a partial final output word. */
  ag_cache_range out={spans[3].offset,(spans[3].bytes+31u)&~31u};
  if(overlap(out,spans[q]))return NPU_RAW_ARGUMENT;
 }
 volatile npu_dma_regs *r=c->config.device;
 if(rd(&r->id)!=NPU_DMA_ID_VALUE)return NPU_RAW_DEVICE;
 if(rd(&r->status)&NPU_DMA_BUSY)return NPU_RAW_BUSY;
 owner=c;memcpy(lease,spans,sizeof lease);c->active=1;c->result.valid=0;
 c->lease_job=j.raw;c->lease_job.partial_offset=j.output_offset;c->lease_job.output_stride=4;
 c->lease_layout=l;c->lease_layout.output_stride=4;c->lease_layout.partial_span=j.raw.rows*4u;
 if(ag_cache_maintain_ranges(lease,4,budget.cache_polls))return unknown(c);
 if(rd(&r->status)&NPU_DMA_BUSY)return unknown(c);
 wr(&r->ctrl,NPU_DMA_CLEAR);barrier();
 wr(&r->n,j.raw.rows);wr(&r->k,j.raw.cols);wr(&r->weight_lo,j.raw.weight_offset);wr(&r->weight_hi,0);
 wr(&r->activation_lo,j.raw.activation_offset);wr(&r->activation_hi,0);wr(&r->output_lo,j.output_offset);wr(&r->output_hi,0);
 wr(&r->weight_stride,l.weight_stride);wr(&r->activation_stride,l.activation_stride);wr(&r->output_stride,4);
 wr(extra(r,NPU_FUSED_SCALE),j.scale_offset);wr(extra(r,NPU_FUSED_MAXIMUM),j.maximum);wr(extra(r,NPU_FUSED_MODE),1);
 barrier();wr(&r->ctrl,NPU_DMA_START);barrier();
 while(budget.device_polls--){status=rd(&r->status);if(!(status&NPU_DMA_BUSY)&&(status&(NPU_DMA_DONE|NPU_DMA_ERROR))){terminal=1;break;}}
 if(!terminal)return unknown(c);
 result.error=rd(&r->error_code);result.cycles=rd(&r->cycle_count);result.result_count=rd(&r->result_count);
 result.numeric.status=rd(extra(r,NPU_FUSED_FLAGS));result.numeric.saturations=rd(extra(r,NPU_FUSED_SATURATIONS));
 result.numeric.underflows=rd(extra(r,NPU_FUSED_UNDERFLOWS));result.numeric.invalid_inputs=rd(extra(r,NPU_FUSED_INVALID));
 if(ag_cache_maintain_ranges(&lease[3],1,budget.cache_polls))return unknown(c);
 if(rd(&r->status)&NPU_DMA_BUSY)return unknown(c);
 /* Do not leave mode1 selected for existing raw clients sharing this device. */
 wr(extra(r,NPU_FUSED_MODE),0);barrier();
 c->active=0;c->cache_unknown=0;c->device_error=result.error;owner=0;*stats=result;
 c->last_status=((status&NPU_DMA_ERROR)||result.error||result.result_count!=j.raw.rows)?NPU_RAW_DEVICE:NPU_RAW_OK;
 return c->last_status;
#endif
}
int npu_fused_matrix_recover(npu_raw_context *c,uint32_t polls){
#if !defined(__riscv) && !defined(NPU_FUSED_MATRIX_TEST_MMIO)
 (void)c;(void)polls;return NPU_RAW_HOST_UNSUPPORTED;
#else
 if(!c||owner!=c||!c->active||!polls)return NPU_RAW_ARGUMENT;
 volatile npu_dma_regs *r=c->config.device;
 if(rd(&r->status)&NPU_DMA_BUSY)return unknown(c);
 if(ag_cache_maintain_ranges(lease,4,polls))return unknown(c);
 if(rd(&r->status)&NPU_DMA_BUSY)return unknown(c);
 wr(&r->ctrl,NPU_DMA_CLEAR);wr(extra(r,NPU_FUSED_MODE),0);barrier();
 c->active=0;c->cache_unknown=0;c->result.valid=0;owner=0;c->last_status=NPU_RAW_OK;return 0;
#endif
}
