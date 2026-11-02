#include "npu_tensor_driver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char memory[512];
int main(void) {
    npu_tensor_context ctx;
    npu_tensor_config cfg={0};
    npu_tensor_region grants[2]={{0,64,NPU_TENSOR_READ},{64,64,NPU_TENSOR_WRITE}};
    npu_tensor_job job={0};npu_tensor_stats stats;
    cfg.mode=NPU_TENSOR_SOFTWARE_REFERENCE;
    cfg.mapping.data=memory;cfg.mapping.capacity=sizeof memory;cfg.mapping.ddr_base=0;
    cfg.regions=grants;cfg.region_count=2;
    memset(memory,0x55,sizeof memory);
    {int32_t x=65536;memcpy(memory,&x,4);}
    assert(npu_tensor_init(&ctx,&cfg)==0);
    job.opcode=NPU_TENSOR_COPY;job.n=1;job.a=0;job.out=64;
    assert(npu_tensor_execute(&ctx,&job,&stats)==0);
    assert(!memcmp(memory,memory+64,4));
    assert(!ctx.active && ctx.jobs_completed==1);
    job.out=128;memset(memory+128,0x33,32);
    assert(npu_tensor_execute(&ctx,&job,&stats)==NPU_TENSOR_DRIVER_REGION);
    assert(memory[128]==0x33 && ctx.jobs_completed==1);
    job.out=64;ctx.active=1;
    assert(npu_tensor_execute(&ctx,&job,&stats)==NPU_TENSOR_DRIVER_BUSY);
    assert(ctx.active);
    assert(npu_tensor_recover(&ctx)==0 && !ctx.active);
    job.out=0;
    assert(npu_tensor_execute(&ctx,&job,&stats)!=0);
    assert(ctx.jobs_completed==1);
    cfg.mapping.capacity=1u<<30;cfg.mapping.ddr_base=32;
    assert(npu_tensor_init(&ctx,&cfg)==NPU_TENSOR_DRIVER_ARGUMENT);
    cfg.mapping.capacity=sizeof memory;cfg.mapping.ddr_base=0;
    assert(npu_tensor_init(&ctx,&cfg)==0);
    job.opcode=NPU_TENSOR_COPY;job.n=1;job.a=0;job.out=64;
    memset(&stats,0xa5,sizeof stats);
    npu_tensor_stats untouched=stats;
    assert(npu_tensor_submit(&ctx,&job)==0);
    assert(ctx.active && ctx.jobs_started==1 && ctx.jobs_completed==0);
    assert(!memcmp(&stats,&untouched,sizeof stats));
    job.n=0; /* Accepted descriptors are snapshots; no revalidation on poll. */
    assert(npu_tensor_poll(&ctx,0)==0);
    assert(npu_tensor_wait(&ctx,0)==0);
    assert(npu_tensor_submit(&ctx,&job)==NPU_TENSOR_DRIVER_BUSY);
    assert(npu_tensor_complete(&ctx,(npu_tensor_stats *)&ctx)==NPU_TENSOR_DRIVER_ARGUMENT);
    assert(ctx.active);
    assert(npu_tensor_complete(&ctx,&stats)==0);
    assert(!ctx.active && ctx.jobs_completed==1 && stats.error==0);
    untouched=stats;
    assert(npu_tensor_complete(&ctx,&stats)==NPU_TENSOR_DRIVER_ARGUMENT);
    assert(!memcmp(&stats,&untouched,sizeof stats));
    cfg.mapping.capacity=4;
    assert(npu_tensor_init(&ctx,&cfg)==0);
    job.n=1;
    memset(&stats,0xa5,sizeof stats);untouched=stats;
    assert(npu_tensor_submit(&ctx,&job)==0 && ctx.active);
    assert(!memcmp(&stats,&untouched,sizeof stats));
    assert(npu_tensor_complete(&ctx,&stats)==NPU_TENSOR_ERR_MAPPING);
    assert(!ctx.active && !ctx.jobs_completed && stats.error==NPU_TENSOR_ERR_MAPPING);
    assert(npu_tensor_poll(NULL,1)==NPU_TENSOR_DRIVER_ARGUMENT);
    assert(npu_tensor_wait(NULL,1)==NPU_TENSOR_DRIVER_ARGUMENT);
    assert(npu_tensor_submit(NULL,&job)==NPU_TENSOR_DRIVER_ARGUMENT);
    assert(npu_tensor_complete(NULL,&stats)==NPU_TENSOR_DRIVER_ARGUMENT);
    puts("PASS_TENSOR_DRIVER grant-before-write ownership recovery async-reference");
    return 0;
}
