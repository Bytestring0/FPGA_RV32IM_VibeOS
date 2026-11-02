#ifndef QW_INTERNAL_H
#define QW_INTERNAL_H
#include "qw_runtime.h"
#define QW_MAGIC 0x51575231u
#define QW_FFN 3584u
#define QW_LINEAR_LAYERS 18u
#define QW_ATTN_LAYERS 6u
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
typedef struct {
 const uint32_t *tokens;size_t count,done;
 uint32_t active,stage,layer,row,index,head,mat_ready,want_logits;
 uint32_t last_poll_units;
 uint64_t last_poll_ms,max_poll_ms;
 float sx,maximum,total;
} qw_execution;
struct qw_model {
 uint32_t magic,position,needs_reset,npu_jobs,logits_valid;
 int last_error;qw_options options;qw_memory_plan plan;
 unsigned char *arena;size_t arena_capacity;
 npu_raw_context raw;npu_raw_region grants[3];npu_package_view package;
 qw_tensor tensors[320];uint32_t tensor_count;qw_layer layers[24];
 qw_tensor *embedding,*final_norm;qw_work *w;float *delta,*conv,*kv,*logits;
 int8_t *activation;unsigned char *partial;
 /* Bound identity and the exact successfully processed prefix are model-owned. */
 uint8_t parameter_sha256[96];
 uint32_t processed_ids[QW_CONTEXT_MAX];
 qw_execution execution;
 uint32_t checkpoint_active;
 qw_checkpoint_job *checkpoint_job;
};
float qw_value(const qw_tensor *,uint32_t);
qw_tensor *qw_find(qw_model *,const char *);
int qw_bind_catalog(qw_model *,const char *,size_t);
int qw_matrix(qw_model *,const qw_tensor *,const float *,float *);
#endif
