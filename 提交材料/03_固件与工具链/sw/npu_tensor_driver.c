#include "npu_tensor_driver.h"
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(npu_tensor_job)==64,"tensor v2 descriptor ABI");
_Static_assert(offsetof(npu_tensor_regs,descriptor)==0x20,"tensor v2 CSR ABI");
_Static_assert(offsetof(npu_tensor_regs,numeric_status)==0x60,"tensor v2 diagnostics ABI");

enum { PHASE_IDLE=0,PHASE_SUBMITTED=1,PHASE_READY=2,PHASE_UNKNOWN=3 };
/* Single-thread/non-ISR ownership. A callback cannot borrow another context to
 * submit or recover the same physical accelerator, even after hardware DONE. */
static npu_tensor_context *hardware_owner;
static npu_tensor_context *api_owner;
#if defined(__riscv) || defined(NPU_TENSOR_TEST_MMIO)
#define TENSOR_MMIO 1
static void barrier(void) {
#if defined(__riscv)
 __asm__ volatile("fence iorw,iorw" ::: "memory");
#else
 __asm__ volatile("" ::: "memory");
#endif
}
#if defined(NPU_TENSOR_TEST_MMIO)
/* Dedicated test executable supplies a volatile CSR model. Normal host builds
 * never enable the hardware path. These symbols are absent from target builds. */
extern uint32_t npu_tensor_test_read32(volatile uint32_t *);
extern void npu_tensor_test_write32(volatile uint32_t *,uint32_t);
static uint32_t read32(volatile uint32_t *p) {return npu_tensor_test_read32(p);}
static void write32(volatile uint32_t *p,uint32_t v) {npu_tensor_test_write32(p,v);}
#else
static uint32_t read32(volatile uint32_t *p) {return *p;}
static void write32(volatile uint32_t *p,uint32_t v) {*p=v;}
#endif
#else
#define TENSOR_MMIO 0
#endif
static int native_overlap(const void *a,size_t na,const void *b,size_t nb) {
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 if(!na||!nb)return 0;
 if(x>UINTPTR_MAX-na||y>UINTPTR_MAX-nb)return 1;
 return x<y+nb&&y<x+na;
}
static int regions_valid(const npu_tensor_config *c) {
 size_t i;
 if(!c->regions||!c->region_count)return 0;
 for(i=0;i<c->region_count;i++) {
  const npu_tensor_region *r=c->regions+i;
  if(!r->bytes||!r->access||(r->access&~3u)||
     ((r->offset|r->bytes)&31u)||
     (uint64_t)r->offset+r->bytes>NPU_TENSOR_DDR_LIMIT)return 0;
 }
 return 1;
}
static int granted(const npu_tensor_config *c,const npu_tensor_layout *l) {
 uint32_t i;size_t r;
 for(i=0;i<l->span_count;i++) {
  const npu_tensor_span *s=l->spans+i;
  uint64_t end=((uint64_t)s->offset+s->bytes+31u)&~UINT64_C(31);
  int found=0;
  for(r=0;r<c->region_count;r++) {
   const npu_tensor_region *g=c->regions+r;
   if((g->access&s->access)==s->access&&s->offset>=g->offset&&
      end<=(uint64_t)g->offset+g->bytes){found=1;break;}
  }
  if(!found)return 0;
 }
 return 1;
}
static int stats_alias(const npu_tensor_context *c,const npu_tensor_job *j,
 const npu_tensor_stats *s) {
 return native_overlap(s,sizeof *s,c,sizeof *c)||
  (j&&native_overlap(s,sizeof *s,j,sizeof *j))||
  native_overlap(s,sizeof *s,c->config.regions,
   c->config.region_count*sizeof *c->config.regions)||
  (c->config.mode==NPU_TENSOR_SOFTWARE_REFERENCE&&
   native_overlap(s,sizeof *s,c->config.mapping.data,c->config.mapping.capacity));
}
static int enter(npu_tensor_context *c) {
 if(!c)return NPU_TENSOR_DRIVER_ARGUMENT;
 if(api_owner||c->in_call)return NPU_TENSOR_DRIVER_BUSY;
 if(c->config.mode==NPU_TENSOR_HARDWARE&&hardware_owner&&hardware_owner!=c)
  return NPU_TENSOR_DRIVER_BUSY;
 c->in_call=1;api_owner=c;return 0;
}
static int leave(npu_tensor_context *c,int rc) {
 c->in_call=0;api_owner=0;return rc;
}
static int unknown(npu_tensor_context *c,int cache) {
 c->phase=PHASE_UNKNOWN;
 if(cache)c->cache_unknown=1;
 return NPU_TENSOR_DRIVER_UNKNOWN;
}
static void release(npu_tensor_context *c) {
 if(hardware_owner==c)hardware_owner=0;
 c->active=0;c->phase=PHASE_IDLE;c->pending_status=0;c->pending_rc=0;
}
int npu_tensor_init(npu_tensor_context *c,const npu_tensor_config *config) {
 npu_tensor_config in;
 if(!c||!config)return NPU_TENSOR_DRIVER_ARGUMENT;
 /* Check guards without reading a not-yet-initialized context. */
 if(api_owner||hardware_owner==c)return NPU_TENSOR_DRIVER_BUSY;
 if(native_overlap(c,sizeof *c,config,sizeof *config))return NPU_TENSOR_DRIVER_ARGUMENT;
 in=*config;
 if(in.region_count>SIZE_MAX/sizeof *in.regions||!regions_valid(&in))
  return NPU_TENSOR_DRIVER_ARGUMENT;
 if(native_overlap(c,sizeof *c,in.regions,in.region_count*sizeof *in.regions))
  return NPU_TENSOR_DRIVER_ARGUMENT;
 if(in.mode==NPU_TENSOR_SOFTWARE_REFERENCE) {
  if(!in.mapping.data||!in.mapping.capacity||
     in.mapping.ddr_base>=NPU_TENSOR_DDR_LIMIT||
     in.mapping.capacity>NPU_TENSOR_DDR_LIMIT-in.mapping.ddr_base||
     (uintptr_t)in.mapping.data>UINTPTR_MAX-in.mapping.capacity||
     native_overlap(c,sizeof *c,in.mapping.data,in.mapping.capacity))
   return NPU_TENSOR_DRIVER_ARGUMENT;
 } else if(in.mode==NPU_TENSOR_HARDWARE) {
  if(!in.regs||!in.publish||!in.acquire||!in.quiescent||
     !in.device_polls||!in.cache_polls)return NPU_TENSOR_DRIVER_ARGUMENT;
 } else return NPU_TENSOR_DRIVER_ARGUMENT;
 memset(c,0,sizeof *c);c->config=in;return 0;
}
static int submit_locked(npu_tensor_context *c,const npu_tensor_job *job) {
 npu_tensor_job j;npu_tensor_layout layout;uint32_t rc;
 if(!job)return NPU_TENSOR_DRIVER_ARGUMENT;
 if(c->active)return NPU_TENSOR_DRIVER_BUSY;
 j=*job;rc=npu_tensor_query(&j,&layout);if(rc)return (int)rc;
 if(!regions_valid(&c->config)||!granted(&c->config,&layout))return NPU_TENSOR_DRIVER_REGION;
 if(c->config.mode==NPU_TENSOR_SOFTWARE_REFERENCE) {
#if defined(__riscv) && !defined(NPU_TENSOR_REFERENCE_ON_TARGET)
  /* The host oracle has a98KiB arithmetic frame. It remains host-only. */
  return NPU_TENSOR_DRIVER_HOST;
#else
  npu_tensor_stats result={0};
  c->lease=j;c->layout=layout;c->active=1;c->jobs_started++;
  rc=npu_tensor_reference(&j,&c->config.mapping,&result);
  c->pending_stats=result;c->pending_rc=(int)rc;
  c->pending_status=2;c->phase=PHASE_READY;c->last_cycles=0;return 0;
#endif
 }
#if !TENSOR_MMIO
 return NPU_TENSOR_DRIVER_HOST;
#else
 {
  volatile npu_tensor_regs *r=c->config.regs;uint32_t words[16];unsigned i;
  if(hardware_owner&&hardware_owner!=c)return NPU_TENSOR_DRIVER_BUSY;
  if(read32(&r->id)!=NPU_TENSOR_ID||read32(&r->version)!=NPU_TENSOR_NUMERIC_VERSION)
   return NPU_TENSOR_DRIVER_DEVICE;
  if(read32(&r->status)&1u)return NPU_TENSOR_DRIVER_BUSY;
  c->lease=j;c->layout=layout;c->active=1;c->jobs_started++;
  c->phase=PHASE_SUBMITTED;c->pending_status=0;c->pending_rc=0;hardware_owner=c;
  if(c->config.publish(c->config.platform_user,&c->layout,c->config.cache_polls))
   return unknown(c,1);
  memcpy(words,&c->lease,sizeof words);
  write32(&r->ctrl,2);barrier();
  for(i=0;i<16;i++)write32(&r->descriptor[i],words[i]);
  barrier();write32(&r->ctrl,1);barrier();return 0;
 }
#endif
}
static int poll_locked(npu_tensor_context *c,uint32_t polls) {
 if(!c->active)return NPU_TENSOR_DRIVER_ARGUMENT;
 if(c->phase==PHASE_UNKNOWN)return NPU_TENSOR_DRIVER_UNKNOWN;
 if(c->phase==PHASE_READY)return 0;
 if(c->phase!=PHASE_SUBMITTED)return NPU_TENSOR_DRIVER_ARGUMENT;
#if !TENSOR_MMIO
 (void)polls;return NPU_TENSOR_DRIVER_HOST;
#else
 while(polls--) {
  uint32_t status=read32(&c->config.regs->status);
  if(!(status&1u)&&(status&6u)) {
   c->pending_status=status;c->phase=PHASE_READY;return 0;
  }
 }
 return NPU_TENSOR_DRIVER_PENDING;
#endif
}
static int wait_locked(npu_tensor_context *c,uint32_t polls) {
 int rc=poll_locked(c,polls);
 return rc==NPU_TENSOR_DRIVER_PENDING?unknown(c,0):rc;
}
static int complete_locked(npu_tensor_context *c,npu_tensor_stats *stats) {
 npu_tensor_stats result={0};int rc;
 if(!stats||stats_alias(c,0,stats))return NPU_TENSOR_DRIVER_ARGUMENT;
 if(!c->active)return NPU_TENSOR_DRIVER_ARGUMENT;
 if(c->phase==PHASE_UNKNOWN)return NPU_TENSOR_DRIVER_UNKNOWN;
 if(c->phase!=PHASE_READY)return NPU_TENSOR_DRIVER_PENDING;
 if(c->config.mode==NPU_TENSOR_SOFTWARE_REFERENCE) {
  result=c->pending_stats;rc=c->pending_rc;
  release(c);if(!rc)c->jobs_completed++;*stats=result;return rc;
 }
#if !TENSOR_MMIO
 return NPU_TENSOR_DRIVER_HOST;
#else
 {
  volatile npu_tensor_regs *r=c->config.regs;uint32_t status=read32(&r->status);
  /* A second owner or a changed device invalidates readiness; never acquire
   * or publish against a BUSY status even after a prior READY observation. */
  if((status&1u)||!(status&6u))return unknown(c,0);
  c->last_device_error=read32(&r->error);c->last_cycles=read32(&r->cycles);
  result.error=c->last_device_error;result.result=read32(&r->result);
  result.numeric.status=read32(&r->numeric_status);
  result.numeric.domain_clamps=read32(&r->domain_clamps);
  result.numeric.saturations=read32(&r->saturations);
  result.numeric.underflows=read32(&r->underflows);
  result.numeric.zero_vectors=read32(&r->zero_vectors);
  result.numeric.invalid_inputs=read32(&r->invalid_inputs);
  if(c->config.acquire(c->config.platform_user,&c->layout,c->config.cache_polls))
   return unknown(c,1);
  rc=status&4u?NPU_TENSOR_DRIVER_DEVICE:0;
  release(c);if(!rc)c->jobs_completed++;*stats=result;return rc;
 }
#endif
}
int npu_tensor_submit(npu_tensor_context *c,const npu_tensor_job *job) {
 int rc=enter(c);if(rc)return rc;return leave(c,submit_locked(c,job));
}
int npu_tensor_poll(npu_tensor_context *c,uint32_t polls) {
 int rc=enter(c);if(rc)return rc;return leave(c,poll_locked(c,polls));
}
int npu_tensor_wait(npu_tensor_context *c,uint32_t polls) {
 int rc=enter(c);if(rc)return rc;return leave(c,wait_locked(c,polls));
}
int npu_tensor_complete(npu_tensor_context *c,npu_tensor_stats *stats) {
 int rc=enter(c);if(rc)return rc;return leave(c,complete_locked(c,stats));
}
int npu_tensor_execute(npu_tensor_context *c,const npu_tensor_job *job,npu_tensor_stats *stats) {
 int rc=enter(c);if(rc)return rc;
 if(!job||!stats||stats_alias(c,job,stats))return leave(c,NPU_TENSOR_DRIVER_ARGUMENT);
 rc=submit_locked(c,job);
 if(!rc)rc=wait_locked(c,c->config.device_polls);
 if(!rc)rc=complete_locked(c,stats);
 return leave(c,rc);
}
int npu_tensor_recover(npu_tensor_context *c) {
 int rc=enter(c);if(rc)return rc;
 if(c->config.mode==NPU_TENSOR_SOFTWARE_REFERENCE) {
  release(c);c->cache_unknown=0;return leave(c,0);
 }
#if !TENSOR_MMIO
 return leave(c,NPU_TENSOR_DRIVER_HOST);
#else
 {
  volatile npu_tensor_regs *r=c->config.regs;
  c->active=1;c->phase=PHASE_UNKNOWN;hardware_owner=c;
  if(read32(&r->id)!=NPU_TENSOR_ID||read32(&r->version)!=NPU_TENSOR_NUMERIC_VERSION)
   return leave(c,NPU_TENSOR_DRIVER_UNKNOWN);
  if(read32(&r->status)&1u)return leave(c,NPU_TENSOR_DRIVER_UNKNOWN);
  if(c->config.quiescent(c->config.platform_user,c->config.cache_polls)) {
   c->cache_unknown=1;return leave(c,NPU_TENSOR_DRIVER_UNKNOWN);
  }
  if(c->config.acquire(c->config.platform_user,&c->layout,c->config.cache_polls)) {
   c->cache_unknown=1;return leave(c,NPU_TENSOR_DRIVER_UNKNOWN);
  }
  if(read32(&r->status)&1u)return leave(c,NPU_TENSOR_DRIVER_UNKNOWN);
  write32(&r->ctrl,2);barrier();release(c);c->cache_unknown=0;return leave(c,0);
 }
#endif
}
int npu_tensor_model_execute(void *user,const npu_tensor_job *j,npu_tensor_stats *s) {
 int rc=npu_tensor_execute((npu_tensor_context *)user,j,s);
 if(rc==NPU_TENSOR_DRIVER_UNKNOWN)return -106;
 return rc?-105:0;
}
