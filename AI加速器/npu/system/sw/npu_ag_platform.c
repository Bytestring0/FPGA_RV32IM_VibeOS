#include "npu_ag_platform.h"
#include "npu_runtime.h"

static int publish(void *user,const npu_raw_job *job,const npu_raw_layout *layout,uint32_t polls) {
    ag_cache_range ranges[3];(void)user;
    if(!job||!layout)return -1;
    ranges[0]=(ag_cache_range){job->weight_offset,layout->weight_span};
    ranges[1]=(ag_cache_range){job->activation_offset,layout->activation_read_span};
    ranges[2]=(ag_cache_range){job->partial_offset,layout->partial_span};
    return ag_cache_maintain_ranges(ranges,3,polls);
}
static int acquire(void *user,const npu_raw_job *job,const npu_raw_layout *layout,uint32_t polls) {
    (void)user;
    if(!job||!layout)return -1;
    return ag_cache_maintain_range(job->partial_offset,layout->partial_span,polls);
}
static int recover(void *user,uint32_t polls) {
    (void)user;
#if defined(__riscv)
    while(polls--) {
        uint32_t s=*(volatile uint32_t *)0x10000200u;
        if(!(s&1u)||(s&0x12u))return -1;
        if(!(s&8u))return 0;
    }
#else
    (void)polls;
#endif
    return -1;
}
int npu_ag_open(npu_raw_context *ctx,volatile npu_dma_regs *device,
                const npu_raw_region *regions,size_t count) {
    npu_raw_config config={0};
    config.mode=NPU_RAW_HARDWARE;config.device=device;
    config.platform.user=0;config.platform.publish=publish;
    config.platform.acquire=acquire;config.platform.recover=recover;
    config.regions=regions;config.region_count=count;
    return npu_raw_init(ctx,&config);
}
