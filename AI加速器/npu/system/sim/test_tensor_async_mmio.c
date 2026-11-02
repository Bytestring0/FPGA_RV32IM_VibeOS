/* Driver contract only: a volatile CSR model, never tensor numerical output.
 * NPU_TENSOR_TEST_MMIO explicitly enables this seam; ordinary host remains HOST.
 * This source also cross-compiles as RV32 freestanding firmware. */
#include "npu_tensor_driver.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if !defined(__riscv)
#include <stdio.h>
#include <stdlib.h>
#endif
static unsigned assertions;
static void check(int condition,unsigned line) {
 assertions++;
 if(condition)return;
#if defined(__riscv)
 *(volatile uint32_t *)(uintptr_t)0x10000004u=line?line:1;
 for(;;){}
#else
 fprintf(stderr,"ASYNC_CONTRACT_FAIL line=%u\n",line);exit(1);
#endif
}
#define CHECK(x) check(!!(x),__LINE__)
static npu_tensor_regs regs;
static npu_tensor_context context,other;
static npu_tensor_region regions[2]={{0,64,1},{64,64,2}};
static npu_tensor_job job;
static npu_tensor_stats stats,sentinel;
static npu_tensor_config config;
static unsigned reads,status_reads,writes,starts,clears,descriptors;
static unsigned publishes,acquires,quiets,busy_left;
static int never_finish,publish_fail,acquire_fail,quiet_fail,reenter;
static uint32_t finish_status;
static uint32_t submitted[16];
static void unchanged(void) {CHECK(!memcmp(&stats,&sentinel,sizeof stats));}
static void fill_result(void) {
 regs.error=(finish_status&4u)?10u:0u;regs.result=0x1234;regs.cycles=77;
 regs.numeric_status=7;regs.domain_clamps=3;regs.saturations=2;
 regs.underflows=4;regs.zero_vectors=5;regs.invalid_inputs=6;
}
uint32_t npu_tensor_test_read32(volatile uint32_t *p) {
 CHECK((uintptr_t)p>=(uintptr_t)&regs && (uintptr_t)p+4<=(uintptr_t)&regs+sizeof regs);
 reads++;
 if(p==&regs.status) {
  status_reads++;
  if((regs.status&1u)&&starts&&!never_finish) {
   if(busy_left)busy_left--;
   else {regs.status=finish_status;fill_result();}
  }
 }
 return *p;
}
void npu_tensor_test_write32(volatile uint32_t *p,uint32_t value) {
 CHECK((uintptr_t)p>=(uintptr_t)&regs && (uintptr_t)p+4<=(uintptr_t)&regs+sizeof regs);
 writes++;*p=value;
 if(p==&regs.ctrl) {
  if(value==2){clears++;regs.status=0;}
  else if(value==1){starts++;regs.status=1;for(unsigned i=0;i<16;i++)submitted[i]=regs.descriptor[i];}
  else CHECK(0);
 } else {
  CHECK((uintptr_t)p>=(uintptr_t)&regs.descriptor[0]&&(uintptr_t)p<=(uintptr_t)&regs.descriptor[15]);
  descriptors++;
 }
}
static void callback_contract(void) {
 CHECK(context.active && context.in_call);unchanged();
 if(reenter) {
  unsigned before_reads=reads,before_writes=writes;
  CHECK(npu_tensor_submit(&context,&job)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_poll(&context,100)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_wait(&context,100)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_execute(&context,&job,&stats)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_recover(&context)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_submit(&other,&job)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_poll(&other,100)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_recover(&other)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_init(&context,&config)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(npu_tensor_init(&other,&config)==NPU_TENSOR_DRIVER_BUSY);
  CHECK(reads==before_reads && writes==before_writes);unchanged();
 }
}
static int publish(void *u,const npu_tensor_layout *layout,uint32_t polls) {
 CHECK(u==&context && layout==&context.layout && polls==32);
 publishes++;callback_contract();return publish_fail;
}
static int acquire(void *u,const npu_tensor_layout *layout,uint32_t polls) {
 CHECK(u==&context && layout==&context.layout && polls==32);
 acquires++;callback_contract();return acquire_fail;
}
static int quiet(void *u,uint32_t polls) {
 CHECK(u==&context && polls==32);quiets++;callback_contract();return quiet_fail;
}
static void setup(void) {
 memset(&regs,0,sizeof regs);regs.id=NPU_TENSOR_ID;regs.version=NPU_TENSOR_NUMERIC_VERSION;
 memset(&job,0,sizeof job);job.opcode=NPU_TENSOR_COPY;job.n=1;job.out=64;
 memset(&stats,0xa5,sizeof stats);sentinel=stats;
 reads=status_reads=writes=starts=clears=descriptors=publishes=acquires=quiets=0;
 busy_left=2;never_finish=publish_fail=acquire_fail=quiet_fail=reenter=0;finish_status=2;
 memset(&config,0,sizeof config);config.mode=NPU_TENSOR_HARDWARE;config.regs=&regs;
 config.regions=regions;config.region_count=2;config.publish=publish;config.acquire=acquire;config.quiescent=quiet;
 config.platform_user=&context;config.device_polls=16;config.cache_polls=32;
 CHECK(npu_tensor_init(&context,&config)==0);CHECK(npu_tensor_init(&other,&config)==0);
}
static void restore(void) {
 never_finish=0;busy_left=0;regs.status=2;publish_fail=acquire_fail=quiet_fail=0;
 CHECK(npu_tensor_recover(&context)==0);CHECK(!context.active&&!context.cache_unknown&&!context.in_call);unchanged();
}
static void validation(void) {
 setup();job.flags=1;
 CHECK(npu_tensor_submit(&context,&job)==NPU_TENSOR_ERR_STRIDE);
 CHECK(!context.active&&!publishes&&!starts&&!writes);unchanged();
 job.flags=0;job.out=128;CHECK(npu_tensor_submit(&context,&job)==NPU_TENSOR_DRIVER_REGION);
 job.out=64;regs.id=0;CHECK(npu_tensor_submit(&context,&job)==NPU_TENSOR_DRIVER_DEVICE);
 regs.id=NPU_TENSOR_ID;regs.status=1;never_finish=1;
 CHECK(npu_tensor_submit(&context,&job)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(!context.active&&!publishes&&!starts&&!writes);unchanged();
}
static void asynchronous(void) {
 setup();reenter=1;busy_left=4;
 npu_tensor_job original=job;
 CHECK(npu_tensor_submit(&context,&job)==0);
 CHECK(context.active&&context.jobs_started==1&&!context.jobs_completed);
 CHECK(publishes==1&&starts==1&&descriptors==16&&clears==1);
 CHECK(!memcmp(submitted,&original,sizeof original));unchanged();
 job.n=0;job.a=1; /* A caller may change its original descriptor after submit. */
 unsigned before=status_reads;
 CHECK(npu_tensor_poll(&context,0)==NPU_TENSOR_DRIVER_PENDING);CHECK(status_reads==before);
 CHECK(npu_tensor_poll(&context,2)==NPU_TENSOR_DRIVER_PENDING);CHECK(status_reads==before+2);
 CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_PENDING);unchanged();CHECK(acquires==0);
 CHECK(npu_tensor_submit(&context,&original)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(npu_tensor_submit(&other,&original)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(npu_tensor_recover(&other)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(npu_tensor_init(&context,&config)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(npu_tensor_poll(&context,2)==NPU_TENSOR_DRIVER_PENDING);CHECK(status_reads==before+4);
 CHECK(npu_tensor_poll(&context,1)==0);CHECK(status_reads==before+5);
 CHECK(context.active&&!context.jobs_completed&&acquires==0);unchanged();
 before=status_reads;CHECK(npu_tensor_poll(&context,100)==0);CHECK(status_reads==before);
 CHECK(npu_tensor_submit(&other,&original)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(npu_tensor_complete(&context,(npu_tensor_stats *)&context)==NPU_TENSOR_DRIVER_ARGUMENT);
 CHECK(npu_tensor_complete(&context,(npu_tensor_stats *)regions)==NPU_TENSOR_DRIVER_ARGUMENT);
 CHECK(context.active);unchanged();
 CHECK(npu_tensor_complete(&context,&stats)==0);
 CHECK(!context.active&&!context.in_call&&context.jobs_completed==1&&acquires==1);
 CHECK(stats.result==0x1234&&stats.error==0&&context.last_cycles==77);
 CHECK(stats.numeric.status==7&&stats.numeric.domain_clamps==3&&stats.numeric.saturations==2);
 CHECK(stats.numeric.underflows==4&&stats.numeric.zero_vectors==5&&stats.numeric.invalid_inputs==6);
 sentinel=stats;CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_ARGUMENT);unchanged();
 /* The physical owner is released only after complete succeeds. */
 config.platform_user=&context;CHECK(npu_tensor_init(&context,&config)==0);
}
static void pending_and_unknown(void) {
 setup();never_finish=1;
 CHECK(npu_tensor_submit(&context,&job)==0);
 npu_tensor_job lease=context.lease;npu_tensor_layout layout=context.layout;
 unsigned before=status_reads;
 CHECK(npu_tensor_poll(&context,3)==NPU_TENSOR_DRIVER_PENDING);
 CHECK(npu_tensor_poll(&context,2)==NPU_TENSOR_DRIVER_PENDING);
 CHECK(status_reads==before+5&&context.active&&!context.cache_unknown);unchanged();
 CHECK(npu_tensor_wait(&context,2)==NPU_TENSOR_DRIVER_UNKNOWN);CHECK(status_reads==before+7);
 CHECK(!memcmp(&lease,&context.lease,sizeof lease)&&!memcmp(&layout,&context.layout,sizeof layout));
 before=status_reads;
 CHECK(npu_tensor_poll(&context,100)==NPU_TENSOR_DRIVER_UNKNOWN);CHECK(status_reads==before);
 CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_UNKNOWN);unchanged();
 CHECK(npu_tensor_submit(&other,&job)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(npu_tensor_recover(&other)==NPU_TENSOR_DRIVER_BUSY);
 CHECK(npu_tensor_recover(&context)==NPU_TENSOR_DRIVER_UNKNOWN);CHECK(!quiets&&!acquires&&clears==1);
 never_finish=0;busy_left=0;regs.status=2;quiet_fail=1;
 CHECK(npu_tensor_recover(&context)==NPU_TENSOR_DRIVER_UNKNOWN&&context.cache_unknown);unchanged();
 quiet_fail=0;acquire_fail=1;
 CHECK(npu_tensor_recover(&context)==NPU_TENSOR_DRIVER_UNKNOWN&&context.active);unchanged();
 restore();CHECK(context.jobs_started==1&&!context.jobs_completed);
 setup();CHECK(npu_tensor_submit(&context,&job)==0);before=status_reads;
 CHECK(npu_tensor_wait(&context,0)==NPU_TENSOR_DRIVER_UNKNOWN);CHECK(status_reads==before);unchanged();restore();
}
static void cache_failures(void) {
 setup();publish_fail=1;
 CHECK(npu_tensor_submit(&context,&job)==NPU_TENSOR_DRIVER_UNKNOWN);
 CHECK(context.active&&context.cache_unknown&&publishes==1&&!starts&&!writes);unchanged();
 CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_UNKNOWN);unchanged();
 CHECK(npu_tensor_submit(&other,&job)==NPU_TENSOR_DRIVER_BUSY);restore();
 setup();acquire_fail=1;CHECK(npu_tensor_submit(&context,&job)==0);
 CHECK(npu_tensor_poll(&context,10)==0);CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_UNKNOWN);
 CHECK(context.active&&context.cache_unknown&&!context.jobs_completed&&acquires==1);unchanged();
 CHECK(npu_tensor_submit(&other,&job)==NPU_TENSOR_DRIVER_BUSY);restore();
}
static void device_error_and_readiness(void) {
 setup();finish_status=4;CHECK(npu_tensor_submit(&context,&job)==0);
 CHECK(npu_tensor_poll(&context,10)==0);
 CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_DEVICE);
 CHECK(stats.error==10&&context.last_device_error==10&&!context.active&&!context.jobs_completed&&acquires==1);
 setup();never_finish=1;CHECK(npu_tensor_submit(&context,&job)==0);
 regs.status=5;CHECK(npu_tensor_poll(&context,1)==NPU_TENSOR_DRIVER_PENDING);unchanged();
 regs.status=2;CHECK(npu_tensor_poll(&context,1)==0);regs.status=1;
 CHECK(npu_tensor_complete(&context,&stats)==NPU_TENSOR_DRIVER_UNKNOWN);CHECK(!acquires&&context.active);unchanged();restore();
}
static void synchronous_compatibility(void) {
 setup();CHECK(npu_tensor_execute(&context,&job,&stats)==0);
 CHECK(context.jobs_started==1&&context.jobs_completed==1&&!context.active&&publishes==1&&acquires==1);
 CHECK(stats.result==0x1234);
 setup();never_finish=1;context.config.device_polls=2;
 CHECK(npu_tensor_execute(&context,&job,&stats)==NPU_TENSOR_DRIVER_UNKNOWN);unchanged();restore();
 setup();never_finish=1;context.config.device_polls=2;
 CHECK(npu_tensor_model_execute(&context,&job,&stats)==-106);unchanged();restore();
 setup();job.flags=1;CHECK(npu_tensor_execute(&context,&job,&stats)==NPU_TENSOR_ERR_STRIDE);unchanged();
}
int main(void) {
 validation();asynchronous();pending_and_unknown();cache_failures();device_error_and_readiness();synchronous_compatibility();
#if defined(__riscv)
 const char *s="PASS_TENSOR_ASYNC_CONTRACT_RV32\n";
 while(*s)*(volatile uint32_t *)(uintptr_t)0x10000000u=(unsigned char)*s++;
#else
 printf("PASS_TENSOR_ASYNC_CONTRACT assertions=%u poll_bound=1 pending_not_unknown=1 stats_commit_only=1 cross_context_owner=1 callback_reentry=1 timeout_lease=1\n",assertions);
#endif
 return 0;
}
