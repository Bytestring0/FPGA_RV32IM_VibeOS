#ifndef NPU_TENSOR_DRIVER_H
#define NPU_TENSOR_DRIVER_H
#include "npu_tensor_v2.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NPU_TENSOR_CSR_BASE UINT32_C(0x10006000)
enum { NPU_TENSOR_SOFTWARE_REFERENCE=0,NPU_TENSOR_HARDWARE=1 };
enum { NPU_TENSOR_DRIVER_ARGUMENT=-200,NPU_TENSOR_DRIVER_REGION=-201,
 NPU_TENSOR_DRIVER_BUSY=-202,NPU_TENSOR_DRIVER_DEVICE=-203,
 NPU_TENSOR_DRIVER_UNKNOWN=-204,NPU_TENSOR_DRIVER_HOST=-205,
 /* Positive descriptor errors occupy1..20; PENDING is not an error/timeout. */
 NPU_TENSOR_DRIVER_PENDING=256 };
typedef struct { uint32_t offset,bytes,access; } npu_tensor_region;
typedef struct {
 volatile uint32_t ctrl,status,error,cycles,id,version,result,saturations;
 volatile uint32_t descriptor[16];
 volatile uint32_t numeric_status,domain_clamps,underflows,zero_vectors,invalid_inputs;
} npu_tensor_regs;
typedef int (*npu_tensor_cache_fn)(void *,const npu_tensor_layout *,uint32_t);
typedef int (*npu_tensor_quiescent_fn)(void *,uint32_t);
typedef struct {
 unsigned mode;volatile npu_tensor_regs *regs;
 const npu_tensor_region *regions;size_t region_count;
 npu_tensor_mapping mapping;
 void *platform_user;npu_tensor_cache_fn publish,acquire;
 npu_tensor_quiescent_fn quiescent;
 uint32_t device_polls,cache_polls;
} npu_tensor_config;
typedef struct {
 npu_tensor_config config;npu_tensor_job lease;
 npu_tensor_layout layout;
 uint32_t active,cache_unknown,last_device_error,last_cycles;
 uint64_t jobs_started,jobs_completed;
 /* Driver-private state. Do not mutate while initialized/leased. */
 uint32_t phase,in_call,pending_status;
 int pending_rc;
 npu_tensor_stats pending_stats;
} npu_tensor_context;
/* Single owner. Initialize only a new/quiescent object. An UNKNOWN lease cannot
 * be discarded by reinitialization or accessing the device via another context.
 * Regions and mapping remain caller-owned and immutable for the context life.
 * Grant coverage includes complete32B bus lines, even for a 1-byte output.
 * Hardware ERROR invalidates all outputs/state; reset session before reuse.
 * Timeout does not cancel, clear or release any potentially active DMA/cache.
 */
int npu_tensor_init(npu_tensor_context *,const npu_tensor_config *);
/* submit validates/snapshots once and returns0 after START. publish may block.
 * Software reference computes synchronously but holds its result until complete.
 * Caller may then reuse the original descriptor, but not leased DDR/state.
 * poll performs at most poll_count status reads.0 means READY,256 means still
 * PENDING; neither releases ownership nor writes caller stats. poll_count=0
 * performs no MMIO. A READY context remains leased until complete/recover.
 * complete never waits for device: PENDING/UNKNOWN leave stats untouched. It
 * rechecks READY, acquires cache synchronously and only then publishes stats.
 * DEVICE error publishes error diagnostics after acquire; outputs/state remain
 * invalid. Acquire failure is UNKNOWN and does not publish stats.
 * wait is explicit bounded polling: exhausted PENDING becomes UNKNOWN, retaining
 * all leases. UNKNOWN requires recover; poll/complete cannot legitimize results.
 * One hardware owner is enforced across contexts, including READY and UNKNOWN.
 * APIs are single-thread/non-ISR; callbacks must not reenter ANY driver API,
 * free contexts/regions/DDR or touch the device. Cache callbacks remain blocking;
 * these interfaces do not promise a wall-clock/UI latency bound.
 */
int npu_tensor_submit(npu_tensor_context *,const npu_tensor_job *);
int npu_tensor_poll(npu_tensor_context *,uint32_t poll_count);
int npu_tensor_wait(npu_tensor_context *,uint32_t poll_count);
int npu_tensor_complete(npu_tensor_context *,npu_tensor_stats *);
/* Compatibility: submit + wait(config.device_polls) + complete. */
int npu_tensor_execute(npu_tensor_context *,const npu_tensor_job *,npu_tensor_stats *);
int npu_tensor_recover(npu_tensor_context *);
int npu_tensor_ag_init(npu_tensor_context *,const npu_tensor_region *,size_t,
 uint32_t device_polls,uint32_t cache_polls);
/* qw_tensor_execute_fn compatible. UNKNOWN maps to QW_UNKNOWN=-106; other
 * errors map to QW_DEVICE=-105. No automatic software fallback. */
int npu_tensor_model_execute(void *,const npu_tensor_job *,npu_tensor_stats *);
#ifdef __cplusplus
}
#endif
#endif
