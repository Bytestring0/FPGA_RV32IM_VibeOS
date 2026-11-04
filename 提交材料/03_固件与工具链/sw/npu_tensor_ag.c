#include "npu_tensor_driver.h"
#include "npu_runtime.h"
static int publish(void *u,const npu_tensor_layout *l,uint32_t polls) {
 ag_cache_range ranges[AG_CACHE_MAX_RANGES];size_t count=0;uint32_t i;
 (void)u;if(!l||l->span_count>AG_CACHE_MAX_RANGES)return -1;
 for(i=0;i<l->span_count;i++)if(l->spans[i].bytes) {
  ranges[count++]=(ag_cache_range){l->spans[i].offset,l->spans[i].bytes};
 }
 return ag_cache_maintain_ranges(ranges,count,polls);
}
static int acquire(void *u,const npu_tensor_layout *l,uint32_t polls) {
 ag_cache_range ranges[AG_CACHE_MAX_RANGES];size_t count=0;uint32_t i;
 (void)u;if(!l||l->span_count>AG_CACHE_MAX_RANGES)return -1;
 for(i=0;i<l->span_count;i++)if(l->spans[i].bytes&&(l->spans[i].access&NPU_TENSOR_WRITE)) {
  ranges[count++]=(ag_cache_range){l->spans[i].offset,l->spans[i].bytes};
 }
 return ag_cache_maintain_ranges(ranges,count,polls);
}
static int quiescent(void *u,uint32_t polls) {
 (void)u;
#if defined(__riscv)
 while(polls--) {
  uint32_t status=*(volatile uint32_t *)(uintptr_t)0x10000200u;
  if(!(status&1u)||(status&0x12u))return -1;
  if(!(status&8u))return 0;
 }
#else
 (void)polls;
#endif
 return -1;
}
int npu_tensor_ag_init(npu_tensor_context *context,const npu_tensor_region *regions,
 size_t count,uint32_t device_polls,uint32_t cache_polls) {
 npu_tensor_config config={0};
 config.mode=NPU_TENSOR_HARDWARE;
 config.regs=(volatile npu_tensor_regs *)(uintptr_t)NPU_TENSOR_CSR_BASE;
 config.regions=regions;config.region_count=count;
 config.publish=publish;config.acquire=acquire;config.quiescent=quiescent;
 config.device_polls=device_polls;config.cache_polls=cache_polls;
 return npu_tensor_init(context,&config);
}
