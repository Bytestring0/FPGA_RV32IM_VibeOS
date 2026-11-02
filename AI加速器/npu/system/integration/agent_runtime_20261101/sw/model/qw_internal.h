#ifndef QW_INTERNAL_H
#define QW_INTERNAL_H
#include "qw_runtime.h"
#include "qw_lut_ops.h"
#ifdef QW_ENABLE_TENSOR_V2
#include "qw_tensor_v2.h"
#include "qw_tensor_async.h"
#endif
#define QW_MAGIC 0x51575231u
#define QW_FFN 3584u
#define QW_LINEAR_LAYERS 18u
#define QW_ATTN_LAYERS 6u
#define QW_BASE_W4_TENSORS 186u
#define QW_CPU_TENSORS 134u
#define QW_MAX_TENSORS (QW_BASE_W4_TENSORS+QW_CPU_TENSORS+1u)
enum {QT_W4=1,QT_BF16=2,QT_F32=3};
typedef struct {
 uint32_t id,n,k,count,dtype,stride,ddr;
 const unsigned char *data;const uint16_t *scales;
} qw_tensor;
typedef struct {
 qw_tensor *norm,*post,*gate,*up,*down,*qkv,*z,*a,*b,*conv,*dt,*al,*lnorm,*out;
 qw_tensor *q,*k,*v,*qn,*kn;
} qw_layer;
typedef struct {
 float x[1024],normal[1024],residual[1024],qkv[6144],z[2048],a[16],b[16];
 float attention[2048],gate[3584],up[3584],out[1024],norm_weight[1024],scores[QW_CONTEXT_MAX];
} qw_work;
struct qw_model {
 uint32_t magic,position,needs_reset,npu_jobs,logits_valid;
 int last_error;qw_options options;qw_memory_plan plan;
 unsigned char *arena;size_t arena_capacity;
 npu_raw_context raw;npu_raw_region grants[3];npu_package_view package;
 qw_tensor tensors[QW_MAX_TENSORS];uint32_t tensor_count;qw_layer layers[24];
 uint32_t head_candidate_allowed;qw_tensor *head_candidate,*output_head;
 uint32_t head_integer_enabled,head_integer_numeric_errors;
 uint32_t head_integer_saturations,head_integer_underflows;
 qw_lut_status lut_status;
 qw_tensor *embedding,*final_norm;qw_work *w;float *delta,*conv,*kv,*logits;
 int8_t *activation;unsigned char *partial;
#ifdef QW_ENABLE_TENSOR_V2
 qw_tensor_execute_fn tensor_execute;void *tensor_user,*tensor_scratch;
 size_t tensor_scratch_bytes;uint32_t tensor_ddr;
 qw_tensor_status tensor_status;uint32_t task_active,inside_step;
 qw_tensor_task *task_owner;qw_tensor_task sync_task;
#endif
};
float qw_value(const qw_tensor *,uint32_t);
qw_tensor *qw_find(qw_model *,const char *);
int qw_bind_catalog(qw_model *,const char *,size_t);
int qw_matrix(qw_model *,const qw_tensor *,const float *,float *);
#ifdef QW_ENABLE_TENSOR_V2
int qw_tensor_step(qw_model *,uint32_t,const float **,int);
#endif
#endif
