#ifndef QW_TENSOR_V2_H
#define QW_TENSOR_V2_H
#ifdef QW_ENABLE_TENSOR_V2
#include "qw_runtime.h"
#include "npu_tensor_v2.h"
#include "qw_tensor_async.h"
/* Experimental, uncalibrated fixed-point graph. Compile with QW_ENABLE_TENSOR_V2.
 * No FP arithmetic, allocation or implicit CPU fallback. Delta uses opcode20
 * DELTA_Q24, then RMSNORM with Q48 epsilon 281477120 before returning to Q16;
 * conv/KV remain Q16. Full-model quality is not approved. Callback owns
 * DMA/cache/UNKNOWN handling; QW_UNKNOWN keeps model and scratch storage leased
 * until explicit recovery and reset. */
typedef int (*qw_tensor_execute_fn)(void *,const npu_tensor_job *,npu_tensor_stats *);
typedef struct {
 uint32_t enabled,unknown,numeric_version,last_opcode;
 uint64_t jobs,raw_jobs,tokens;
 npu_fixed_status numeric;
} qw_tensor_status;
size_t qw_tensor_workspace_bytes(void);
int qw_tensor_enable(qw_model *,qw_tensor_execute_fn,void *,void *scratch,
 size_t scratch_bytes,uint32_t scratch_ddr_offset);
int qw_tensor_disable(qw_model *);
int qw_tensor_get_status(const qw_model *,qw_tensor_status *);
/* Call ONLY after the executor has proven hardware/DMA/cache quiescence.
 * This acknowledges recovery, invalidates the session and requires reset. */
int qw_tensor_acknowledge_recovery(qw_model *);
#endif
#endif
