#ifndef QW_TENSOR_ASYNC_H
#define QW_TENSOR_ASYNC_H
#include "qw_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QW_PENDING 256
#define QW_BUSY (-111)
#define QW_TENSOR_TASK_MAGIC UINT32_C(0x5157544b)
enum { QW_TENSOR_TASK_IDLE=0,QW_TENSOR_TASK_RUNNING=1,
 QW_TENSOR_TASK_UNKNOWN=2,QW_TENSOR_TASK_DONE=3,QW_TENSOR_TASK_FAILED=4 };
enum { QW_TENSOR_PHASE_EMBED=0,QW_TENSOR_PHASE_LAYER=1,
 QW_TENSOR_PHASE_FINAL_NORM=2,QW_TENSOR_PHASE_HEAD=3,QW_TENSOR_PHASE_COMMIT=4 };
typedef struct {
 uint32_t magic,state,phase,layer,head_row,token,emit,work_units;
 /* Activation bytes and maxabs are leased together through all head tiles.
  * UNKNOWN retains this state until recovery/discard/reset, zero max is valid. */
 uint32_t head_prepared,head_maximum;
 qw_model *model;
} qw_tensor_task;

/* First use MUST zero-initialize, e.g. qw_tensor_task task = {0}; begin reads
 * magic/state. Caller owns storage through completion or explicit safe discard;
 * never overwrite, zero or move a RUNNING/UNKNOWN task. DONE may be passed to
 * begin again without zeroing. FAILED may be reused without zeroing only after
 * safe discard and the model's required reset. UNKNOWN requires executor
 * recovery, acknowledgement, discard and reset before reuse. */
int qw_tensor_task_begin(qw_tensor_task *task,qw_model *model,uint32_t token,int emit);
/* Each work unit is one graph phase/layer or one <=4096-row output-head tile. */
int qw_tensor_task_advance(qw_tensor_task *task,uint32_t work_quota,
 const float **logits);
/* Unknown device leases must first be recovered and acknowledged. Discard needs reset. */
int qw_tensor_task_discard(qw_tensor_task *task);

#ifdef __cplusplus
}
#endif
#endif
