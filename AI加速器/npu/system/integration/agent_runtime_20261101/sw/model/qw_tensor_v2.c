#ifdef QW_ENABLE_TENSOR_V2
#include "qw_internal.h"
#include "qw_tensor_v2.h"
#include <string.h>
/* This version deliberately preserves the full graph, but declares new
 * rounding: Q16 activations/weights, Q24 recurrence, Q16 decay/beta promoted
 * to Q30, Q16 tile attention accumulation. It is NOT calibrated quality. */
#define WS_BYTES (1024u*1024u)
#define LANES 32768u
/* Every array begins on a 32-byte boundary relative to the aligned workspace. */
typedef struct {
 int32_t x[1024],normal[1024],residual[1024],qkv[6144],z[2048],alpha[16],beta[16];
 int32_t attention[2048],gate[3584],up[3584],project[1024],scores[QW_CONTEXT_MAX];
 int32_t a[LANES],b[LANES*2u],c[8192],out[4096];
 int32_t temp[4096],ones[1024],cosine[32],sine[32];
} tensor_work;
_Static_assert(sizeof(tensor_work)<=WS_BYTES,"tensor workspace exceeds explicit budget");
_Static_assert(QW_CONTEXT_MAX%8==0,"context workspace alignment");
_Static_assert(QW_CONTEXT_MAX<=4096,"tensor attention supports at most 4096 context tokens");
static tensor_work *work(qw_model*m){return (tensor_work*)m->tensor_scratch;}
static uint32_t countadd(uint32_t a,uint32_t b){return b>UINT32_MAX-a?UINT32_MAX:a+b;}
static void merge(npu_fixed_status*a,const npu_fixed_status*b){
 a->status|=b->status;a->domain_clamps=countadd(a->domain_clamps,b->domain_clamps);
 a->saturations=countadd(a->saturations,b->saturations);a->underflows=countadd(a->underflows,b->underflows);
 a->invalid_inputs=countadd(a->invalid_inputs,b->invalid_inputs);a->zero_vectors=countadd(a->zero_vectors,b->zero_vectors);
}
static int32_t clip(qw_model*m,int64_t x){if(x>INT32_MAX||x<INT32_MIN){m->tensor_status.numeric.status|=NPU_FIXED_STATUS_SATURATED;m->tensor_status.numeric.saturations=countadd(m->tensor_status.numeric.saturations,1);return x<0?INT32_MIN:INT32_MAX;}return (int32_t)x;}
static uint64_t divide_rne(uint64_t a,uint64_t b){uint64_t q=a/b,r=a%b;return q+(r>b-r||(r==b-r&&(q&1)));}
static uint32_t pack_q16(int32_t x){
 uint32_t magnitude=x<0?0u-(uint32_t)x:(uint32_t)x;if(!magnitude)return 0;
 unsigned top=0;uint32_t scan=magnitude;while(scan>>1){scan>>=1;top++;}
 uint32_t sig=top>23?(uint32_t)divide_rne(magnitude,UINT64_C(1)<<(top-23)):magnitude<<(23-top);
 if(sig==0x1000000u){sig>>=1;top++;}return (x<0?0x80000000u:0u)|((top+111u)<<23)|(sig&0x7fffffu);
}
static void trace(qw_model*m,unsigned layer,const char*stage,const int32_t*p,unsigned n){
 if(!m->options.trace)return;
 for(unsigned i=0;i<n;i++){uint32_t bits=pack_q16(p[i]);memcpy((unsigned char*)m->w->qkv+(size_t)i*4,&bits,4);}
 m->options.trace(m->options.trace_user,m->position,layer,stage,m->w->qkv,n);
}
static uint16_t le16(const unsigned char*p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const unsigned char*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int32_t value(qw_model*m,const qw_tensor*t,uint32_t i){
 if(t->dtype==QT_BF16)return npu_fixed_bf16_bits_to_q16(le16(t->data+(size_t)i*2),&m->tensor_status.numeric);
 return npu_fixed_f32_bits_to_q16(le32(t->data+(size_t)i*4),&m->tensor_status.numeric);
}
static uint32_t address(qw_model*m,const void*p){
 uintptr_t u=(uintptr_t)p,s=(uintptr_t)m->tensor_scratch,a=(uintptr_t)m->arena;
 if(u>=s&&u-s<m->tensor_scratch_bytes)return m->tensor_ddr+(uint32_t)(u-s);
 if(u>=a&&u-a<m->plan.arena_bytes)return m->options.arena_ddr_offset+(uint32_t)(u-a);
 return UINT32_MAX;
}
static int submit(qw_model*m,npu_tensor_job*j,uint32_t*result){
 npu_tensor_layout layout;npu_tensor_stats stats={0};
 if(m->tensor_status.unknown||m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(npu_tensor_query(j,&layout))return QW_FORMAT;
 m->tensor_status.last_opcode=j->opcode;
 int rc=m->tensor_execute(m->tensor_user,j,&stats);
 if(rc==QW_UNKNOWN){m->tensor_status.unknown=1;return QW_UNKNOWN;}
 merge(&m->tensor_status.numeric,&stats.numeric);
 if(rc||stats.error)return QW_DEVICE;
 m->tensor_status.jobs++;
 if(stats.numeric.status&NPU_FIXED_STATUS_ERROR_MASK)return QW_NUMERIC;
 if(result)*result=stats.result;return 0;
}
static int operation(qw_model*m,uint32_t opcode,int32_t*dst,const int32_t*a,const int32_t*b,uint32_t n){
 tensor_work*w=work(m);if(!n||n>4096)return QW_ARGUMENT;
 memcpy(w->a,a,(size_t)n*4);if(b)memcpy(w->b,b,(size_t)n*4);
 npu_tensor_job j={0};j.opcode=opcode;j.n=n;j.a=address(m,w->a);j.b=address(m,w->b);j.out=address(m,w->out);
 int rc=submit(m,&j,0);if(!rc)memcpy(dst,w->out,(size_t)n*4);return rc;
}
static int norm(qw_model*m,int32_t*dst,const int32_t*src,const qw_tensor*t,uint32_t n,uint32_t centered,uint32_t epsilon,int32_t constant){
 tensor_work*w=work(m);memcpy(w->a,src,(size_t)n*4);
 for(uint32_t i=0;i<n;i++)w->b[i]=t?value(m,t,i):constant;
 npu_tensor_job j={0};j.opcode=NPU_TENSOR_RMSNORM;j.n=n;j.a=address(m,w->a);j.b=address(m,w->b);j.out=address(m,w->out);j.param0=epsilon;j.param1=centered;
 int rc=submit(m,&j,0);if(!rc)memcpy(dst,w->out,(size_t)n*4);return rc;
}
/* The fused device reads the ORIGINAL tagged raw records and FP16 scale bits.
 * CPU submits one row tile; it never loops over individual partials/scales. */
static int matrix_prepare_activation(qw_model*m,const qw_tensor*t,const int32_t*input,uint32_t*maximum){
 if(!t||t->dtype!=QT_W4||!t->k||t->k>4096||!input||!maximum)return QW_FORMAT;
 tensor_work*w=work(m);qw_profile*p=m->options.profile_clock?m->options.profile:0;
 uint64_t start=p?m->options.profile_clock(m->options.profile_clock_user):0;
 memset(m->activation,0,m->plan.activation_bytes);
 memcpy(w->a,input,(size_t)t->k*4);npu_tensor_job quant={0};quant.opcode=NPU_TENSOR_QUANT_A8;quant.n=t->k;quant.a=address(m,w->a);quant.out=address(m,m->activation);
 int rc=submit(m,&quant,maximum);
 if(p)p->quantize_ticks+=m->options.profile_clock(m->options.profile_clock_user)-start;
 return rc;
}
/* Prepared activation/maxabs are one immutable pair. Only the owning model
 * task may retain them across head work units; all raw publish/acquire remain. */
static int matrix_execute_slice(qw_model*m,const qw_tensor*t,int32_t*output,int output_float,uint32_t first_start,uint32_t max_rows,uint32_t maximum,uint32_t*done){
 tensor_work*w=work(m);if(done)*done=0;if(!t||t->dtype!=QT_W4||!t->k||t->k>4096||!max_rows||first_start>=t->n||!m->options.tile_rows)return QW_FORMAT;
 if(max_rows>m->options.tile_rows)max_rows=m->options.tile_rows;if(max_rows>4096u)max_rows=4096u;
 uint32_t end=max_rows>t->n-first_start?t->n:first_start+max_rows;
 qw_profile*p=m->options.profile_clock?m->options.profile:0;uint64_t start=0;
 if(p)p->matrix_calls++;
 int rc;
 for(uint32_t first=first_start;first<end;){
  uint32_t groups=(t->k+127u)/128u,rows=t->n-first;
  if(rows>max_rows)rows=max_rows;
   if(rows>m->options.tile_rows)rows=m->options.tile_rows;
  if(rows>4096u)rows=4096u;
  npu_raw_job raw={0};raw.rows=rows;raw.cols=t->k;raw.weight_offset=t->ddr+first*t->stride;raw.activation_offset=m->grants[1].offset;raw.partial_offset=m->grants[2].offset;raw.weight_stride=t->stride;raw.global_row_base=first;
  npu_raw_layout layout;if(npu_raw_query(&raw,m->grants,3,&layout))return QW_FORMAT;
  if(p){p->jobs_started++;start=m->options.profile_clock(m->options.profile_clock_user);}
  const npu_raw_result_view*v=0;rc=npu_raw_execute(&m->raw,&raw,m->options.budgets,&v);
  if(p)p->rawexecute_ticks+=m->options.profile_clock(m->options.profile_clock_user)-start;
  if(rc==NPU_RAW_UNKNOWN||m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;if(rc)return QW_DEVICE;
  if(!v||!v->valid||v->rows!=rows||v->groups!=groups||v->stride!=layout.output_stride||v->physical_ddr_offset!=raw.partial_offset||v->global_row_base!=first||v->result_count!=layout.expected_result_count)return QW_DEVICE;
  m->npu_jobs++;m->tensor_status.raw_jobs++;
  if(p){p->jobs_completed++;if(m->options.mode==NPU_RAW_HARDWARE){p->hardwarebusy_jobs++;p->hardwarebusy_cycles+=v->cycles;}start=m->options.profile_clock(m->options.profile_clock_user);}
  npu_tensor_job reduce={0};reduce.opcode=NPU_TENSOR_REDUCE_RAW_W4;
  reduce.n=rows;reduce.m=groups;reduce.a=raw.partial_offset;
  const unsigned char *scale_bytes=(const unsigned char*)t->scales+(size_t)first*groups*2u;
  reduce.b=address(m,scale_bytes);
  /* Arbitrary caller-selected row tiles may start inside a32B scale line.
   * Only that exceptional slice is copied; normal aligned4096-row tiles read
   * the immutable package directly. No per-group numerical work on CPU. */
  if(reduce.b&31u){
   size_t bytes=(size_t)rows*groups*2u;
   if(bytes>sizeof w->b)return QW_CAPACITY;
   memcpy(w->b,scale_bytes,bytes);reduce.b=address(m,w->b);
  }
  reduce.out=address(m,w->out);reduce.stride_a=v->stride;reduce.param0=maximum;
  if((rc=submit(m,&reduce,0)))return rc;
  /* pack below is storage conversion only; neither trace nor logits do FP. */
  if(!output_float)memcpy(output+first,w->out,(size_t)rows*4);
  else for(uint32_t r=0;r<rows;r++){
   uint32_t packed=pack_q16(w->out[r]);
   memcpy((unsigned char*)output+(size_t)(first+r)*4,&packed,4);
  }
  if(p)p->scale_reduce_ticks+=m->options.profile_clock(m->options.profile_clock_user)-start;
  first+=rows;
 }
 if(done)*done=end-first_start;return 0;
}
/* Body projections preserve the original per-slice quantization order. */
static int matrix_slice(qw_model*m,const qw_tensor*t,const int32_t*input,int32_t*output,int output_float,uint32_t first_start,uint32_t max_rows,uint32_t*done){
 if(done)*done=0;
 if(!t||t->dtype!=QT_W4||!t->k||t->k>4096||!max_rows||first_start>=t->n||!m->options.tile_rows)return QW_FORMAT;
 uint32_t maximum=0;int rc=matrix_prepare_activation(m,t,input,&maximum);if(rc)return rc;
 return matrix_execute_slice(m,t,output,output_float,first_start,max_rows,maximum,done);
}
static int matrix(qw_model*m,const qw_tensor*t,const int32_t*input,int32_t*output,int output_float){
 if(!t||!t->n)return QW_FORMAT;uint32_t first=0;
 while(first<t->n){uint32_t done=0;int rc=matrix_slice(m,t,input,output,output_float,first,t->n-first,&done);if(rc)return rc;if(!done)return QW_DEVICE;first+=done;}
 return 0;
}
static int rope(qw_model*m,int32_t*x){
 /* Offline round(theta^(-i/32)/(2*pi)*2^32), theta=10000000.
  * Runtime uses integer phase and existing quarter-wave LUT, no pow/sin FP. */
 static const uint32_t phase[32]={683565276u,413076029u,249620353u,150844678u,91154895u,55084574u,33287409u,20115461u,12155701u,7345646u,4438947u,2682440u,1620989u,979558u,591943u,357709u,216162u,130626u,78937u,47701u,28826u,17419u,10526u,6361u,3844u,2323u,1404u,848u,513u,310u,187u,113u};
 tensor_work*w=work(m);for(unsigned i=0;i<32;i++){int32_t s,c;npu_fixed_sincos_q15(phase[i]*m->position,NPU_FIXED_LUT_LINEAR,&s,&c,&m->tensor_status.numeric);w->sine[i]=s*32768;w->cosine[i]=c*32768;}
 memcpy(w->a,x,64u*4);npu_tensor_job j={0};j.opcode=NPU_TENSOR_ROPE;j.n=64;j.a=address(m,w->a);j.b=address(m,w->cosine);j.c=address(m,w->sine);j.out=address(m,w->out);
 int rc=submit(m,&j,0);if(!rc)memcpy(x,w->out,64u*4);return rc;
}
#define TRY(expr) do{int err_=(expr);if(err_)return err_;}while(0)
static int linear(qw_model*m,unsigned layer){
 tensor_work*w=work(m);qw_layer*l=&m->layers[layer];unsigned index=layer-layer/4;
 TRY(matrix(m,l->qkv,w->normal,w->qkv,0));TRY(matrix(m,l->z,w->normal,w->z,0));TRY(matrix(m,l->a,w->normal,w->alpha,0));TRY(matrix(m,l->b,w->normal,w->beta,0));
 trace(m,layer,"linear.qkv",w->qkv,6144);
 for(unsigned first=0;first<6144;first+=512){
  memcpy(w->a,w->qkv+first,512u*4);for(unsigned c=0;c<512;c++)for(unsigned k=0;k<4;k++)w->b[c*4+k]=value(m,l->conv,(first+c)*4+k);
  npu_tensor_job j={0};j.opcode=NPU_TENSOR_DWCONV;j.n=512;j.k=4;j.a=address(m,w->a);j.b=address(m,w->b);j.out=address(m,w->out);j.state=address(m,(int32_t*)m->conv+((size_t)index*6144+first)*3);
  TRY(submit(m,&j,0));memcpy(w->qkv+first,w->out,512u*4);TRY(operation(m,NPU_TENSOR_SILU,w->qkv+first,w->qkv+first,0,512));
 }
 trace(m,layer,"linear.conv",w->qkv,6144);
 for(unsigned h=0;h<16;h++){
  int32_t alpha=clip(m,(int64_t)w->alpha[h]+value(m,l->dt,h)),soft,beta,decay,expal;
  TRY(operation(m,NPU_TENSOR_SOFTPLUS,&soft,&alpha,0,1));TRY(operation(m,NPU_TENSOR_SIGMOID,&beta,w->beta+h,0,1));
  int32_t al=value(m,l->al,h),negative=al>0?-al:al;TRY(operation(m,NPU_TENSOR_EXP_NEG,&expal,&negative,0,1));
  if(al>0){if(expal<=0)return QW_NUMERIC;expal=clip(m,(int64_t)divide_rne(UINT64_C(1)<<32,(uint32_t)expal));}
  TRY(operation(m,NPU_TENSOR_MUL,&decay,&expal,&soft,1));decay=decay==INT32_MIN?INT32_MAX:-decay;TRY(operation(m,NPU_TENSOR_EXP_NEG,&decay,&decay,0,1));
  int32_t*q=w->qkv+h*128,*k=w->qkv+2048+h*128,*v=w->qkv+4096+h*128,*o=w->attention+h*128;
  /* RMS epsilon RNE(4295/128)=34. Weights q=1/128 and k=1/sqrt128.
   * This preserves q/sqrt(d) and normalized k, with declared fixed rounding. */
  TRY(norm(m,q,q,0,128,0,34,512));TRY(norm(m,k,k,0,128,0,34,5793));
  memcpy(w->a,q,128u*4);memcpy(w->b,k,128u*4);memcpy(w->c,v,128u*4);
  npu_tensor_job j={0};j.opcode=NPU_TENSOR_DELTA_Q24;j.n=128;j.a=address(m,w->a);j.b=address(m,w->b);j.c=address(m,w->c);j.out=address(m,w->out);j.state=address(m,(int32_t*)m->delta+((size_t)index*16+h)*128*128);j.param0=(uint32_t)decay*16384u;j.param1=(uint32_t)beta*16384u;
  TRY(submit(m,&j,0));memcpy(o,w->out,128u*4);TRY(norm(m,o,o,l->lnorm,128,0,281477120u,0));TRY(operation(m,NPU_TENSOR_SILU,w->temp,w->z+h*128,0,128));TRY(operation(m,NPU_TENSOR_MUL,o,o,w->temp,128));
 }
 trace(m,layer,"linear.gated",w->attention,2048);
 return matrix(m,l->out,w->attention,w->project,0);
}
static int attention(qw_model*m,unsigned layer){
 tensor_work*w=work(m);qw_layer*l=&m->layers[layer];
 TRY(matrix(m,l->q,w->normal,w->qkv,0));TRY(matrix(m,l->k,w->normal,w->qkv+4096,0));TRY(matrix(m,l->v,w->normal,w->qkv+4608,0));
 int32_t*keys=(int32_t*)m->kv+(size_t)(layer/4)*m->options.context_tokens*1024,*values=keys+(size_t)m->options.context_tokens*512;
 for(unsigned h=0;h<2;h++){int32_t*k=w->qkv+4096+h*256;TRY(norm(m,k,k,l->kn,256,1,4295,0));TRY(rope(m,k));memcpy(keys+(size_t)m->position*512+h*256,k,256u*4);memcpy(values+(size_t)m->position*512+h*256,w->qkv+4608+h*256,256u*4);}
 for(unsigned h=0;h<8;h++){
  int32_t*q=w->qkv+h*512;TRY(norm(m,q,q,l->qn,256,1,4295,0));TRY(rope(m,q));
  for(unsigned first=0;first<=m->position;first+=32){unsigned n=m->position+1-first;if(n>32)n=32;
   for(unsigned r=0;r<n;r++)memcpy(w->a+r*256,keys+(size_t)(first+r)*512+(h/4)*256,256u*4);memcpy(w->b,q,256u*4);
   npu_tensor_job j={0};j.opcode=NPU_TENSOR_MATVEC_Q16;j.n=n;j.k=256;j.a=address(m,w->a);j.b=address(m,w->b);j.out=address(m,w->out);TRY(submit(m,&j,0));memcpy(w->scores+first,w->out,(size_t)n*4);
  }
  for(unsigned p=0;p<=m->position;p++)w->temp[p]=4096;
  TRY(operation(m,NPU_TENSOR_MUL,w->scores,w->scores,w->temp,m->position+1));
  TRY(operation(m,NPU_TENSOR_SOFTMAX,w->scores,w->scores,0,m->position+1));
  memset(w->attention+h*256,0,256u*4);
  for(unsigned first=0;first<=m->position;first+=32){unsigned n=m->position+1-first;if(n>32)n=32;
   memcpy(w->a,w->scores+first,(size_t)n*4);for(unsigned r=0;r<n;r++)memcpy(w->b+r*256,values+(size_t)(first+r)*512+(h/4)*256,256u*4);
   npu_tensor_job j={0};j.opcode=NPU_TENSOR_VECMAT_Q16;j.n=n;j.k=256;j.a=address(m,w->a);j.b=address(m,w->b);j.out=address(m,w->out);TRY(submit(m,&j,0));memcpy(w->temp,w->out,256u*4);TRY(operation(m,NPU_TENSOR_ADD,w->attention+h*256,w->attention+h*256,w->temp,256));
  }
  TRY(operation(m,NPU_TENSOR_SIGMOID,w->temp,q+256,0,256));TRY(operation(m,NPU_TENSOR_MUL,w->attention+h*256,w->attention+h*256,w->temp,256));
 }
 trace(m,layer,"attention.gated",w->attention,2048);
 return matrix(m,l->out,w->attention,w->project,0);
}
size_t qw_tensor_workspace_bytes(void){return WS_BYTES;}
int qw_tensor_enable(qw_model*m,qw_tensor_execute_fn execute,void*user,void*scratch,size_t bytes,uint32_t ddr){
 if(!m||m->magic!=QW_MAGIC||!execute||!scratch||((uintptr_t)scratch&31)||ddr%32)return QW_ARGUMENT;
 if(m->raw.active||m->raw.cache_unknown||m->tensor_status.unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
  if(m->needs_reset||m->position)return QW_NEED_RESET;
 if(!m->output_head||m->output_head!=m->head_candidate||m->output_head->dtype!=QT_W4)return QW_FORMAT;
 if(bytes<WS_BYTES||ddr>=NPU_TENSOR_DDR_LIMIT||bytes>NPU_TENSOR_DDR_LIMIT-ddr)return QW_CAPACITY;
 uintptr_t s=(uintptr_t)scratch,a=(uintptr_t)m->arena;
 if(bytes>UINTPTR_MAX-s||m->plan.arena_bytes>UINTPTR_MAX-a)return QW_ARGUMENT;
 if(s<a+m->plan.arena_bytes&&a<s+bytes)return QW_ARGUMENT;
 if((uint64_t)ddr<(uint64_t)m->options.arena_ddr_offset+m->plan.arena_bytes&&(uint64_t)m->options.arena_ddr_offset<(uint64_t)ddr+bytes)return QW_ARGUMENT;
 m->tensor_execute=execute;m->tensor_user=user;m->tensor_scratch=scratch;m->tensor_scratch_bytes=bytes;m->tensor_ddr=ddr;
 memset(&m->tensor_status,0,sizeof m->tensor_status);m->tensor_status.enabled=1;m->tensor_status.numeric_version=NPU_TENSOR_NUMERIC_VERSION;
 memset(scratch,0,WS_BYTES);memset(m->arena+m->plan.state_offset,0,m->plan.state_bytes);m->logits_valid=0;return 0;
}
int qw_tensor_disable(qw_model*m){
 if(!m||m->magic!=QW_MAGIC)return QW_ARGUMENT;if(m->raw.active||m->raw.cache_unknown||m->tensor_status.unknown)return QW_UNKNOWN;if(m->task_active||m->inside_step)return QW_BUSY;if(m->position||m->needs_reset)return QW_NEED_RESET;
 m->tensor_status.enabled=0;m->tensor_execute=0;memset(m->arena+m->plan.state_offset,0,m->plan.state_bytes);m->logits_valid=0;return 0;
}
int qw_tensor_get_status(const qw_model*m,qw_tensor_status*s){if(!m||m->magic!=QW_MAGIC||!s)return QW_ARGUMENT;*s=m->tensor_status;return 0;}
int qw_tensor_acknowledge_recovery(qw_model*m){if(!m||m->magic!=QW_MAGIC)return QW_ARGUMENT;if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;if(m->inside_step)return QW_BUSY;if(!m->tensor_status.unknown)return QW_ARGUMENT;m->tensor_status.unknown=0;m->needs_reset=1;m->logits_valid=0;return 0;}
static int forward_layer(qw_model*m,unsigned i){
 tensor_work*w=work(m);qw_layer*l=&m->layers[i];
 memcpy(w->residual,w->x,1024u*4);TRY(norm(m,w->normal,w->x,l->norm,1024,1,4295,0));TRY(i%4==3?attention(m,i):linear(m,i));
 TRY(operation(m,NPU_TENSOR_ADD,w->x,w->residual,w->project,1024));TRY(norm(m,w->normal,w->x,l->post,1024,1,4295,0));
 TRY(matrix(m,l->gate,w->normal,w->gate,0));TRY(matrix(m,l->up,w->normal,w->up,0));TRY(operation(m,NPU_TENSOR_SILU,w->gate,w->gate,0,3584));TRY(operation(m,NPU_TENSOR_MUL,w->gate,w->gate,w->up,3584));TRY(matrix(m,l->down,w->gate,w->project,0));TRY(operation(m,NPU_TENSOR_ADD,w->x,w->x,w->project,1024));
 trace(m,i,"layer.output",w->x,1024);return 0;
}
static int task_overlap(const void*a,size_t n,const void*b,size_t k){
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;if(!n||!k)return 0;if(n>UINTPTR_MAX-x||k>UINTPTR_MAX-y)return 1;return x<y+k&&y<x+n;
}
static int task_unknown(const qw_model*m){return m->tensor_status.unknown||m->raw.active||m->raw.cache_unknown;}
int qw_tensor_task_begin(qw_tensor_task*t,qw_model*m,uint32_t token,int emit){
 if(!t||!m||m->magic!=QW_MAGIC||emit<0||emit>1)return QW_ARGUMENT;
 if(task_unknown(m))return QW_UNKNOWN;if(m->task_active||m->inside_step)return QW_BUSY;
 if(t->magic==QW_TENSOR_TASK_MAGIC&&(t->state==QW_TENSOR_TASK_RUNNING||t->state==QW_TENSOR_TASK_UNKNOWN))return t->state==QW_TENSOR_TASK_UNKNOWN?QW_UNKNOWN:QW_BUSY;
 if(t->magic&&t->magic!=QW_TENSOR_TASK_MAGIC)return QW_ARGUMENT;
 if(m->needs_reset)return QW_NEED_RESET;
 if(token>=QW_VOCAB)return QW_TOKEN;if(m->position>=m->options.context_tokens)return QW_CONTEXT_FULL;
 if(!m->tensor_status.enabled||!m->tensor_execute||!m->output_head||m->output_head!=m->head_candidate||m->output_head->dtype!=QT_W4)return QW_FORMAT;
 if(t!=&m->sync_task&&(task_overlap(t,sizeof *t,m,sizeof *m)||task_overlap(t,sizeof *t,m->arena,m->arena_capacity)))return QW_ARGUMENT;
 if(task_overlap(t,sizeof *t,m->tensor_scratch,m->tensor_scratch_bytes)||task_overlap(t,sizeof *t,m->package.buffer,m->package.bytes))return QW_ARGUMENT;
 /* Caller zero-initializes task; live tasks cannot be rebound across models. */
 memset(t,0,sizeof *t);t->magic=QW_TENSOR_TASK_MAGIC;t->state=QW_TENSOR_TASK_RUNNING;t->model=m;t->token=token;t->emit=(uint32_t)emit;
 m->task_active=1;m->task_owner=t;m->logits_valid=0;m->tensor_status.numeric.status&=~NPU_FIXED_STATUS_ERROR_MASK;
 qw_profile*p=m->options.profile_clock?m->options.profile:0;if(p)p->tokens_started++;
 return 0;
}
int qw_tensor_task_advance(qw_tensor_task*t,uint32_t quota,const float**logits){
 if(!t||t->magic!=QW_TENSOR_TASK_MAGIC||!t->model||!logits||!quota)return QW_ARGUMENT;
 qw_model*m=t->model;if(m->magic!=QW_MAGIC)return QW_ARGUMENT;
 if(task_unknown(m))return QW_UNKNOWN;if(m->inside_step)return QW_BUSY;
 if(!m->task_active||m->task_owner!=t)return QW_ARGUMENT;
 if(t->state!=QW_TENSOR_TASK_RUNNING)return m->needs_reset?QW_NEED_RESET:QW_ARGUMENT;
 if(task_overlap(logits,sizeof *logits,t,sizeof *t)||task_overlap(logits,sizeof *logits,m,sizeof *m)||task_overlap(logits,sizeof *logits,m->arena,m->arena_capacity)||task_overlap(logits,sizeof *logits,m->tensor_scratch,m->tensor_scratch_bytes))return QW_ARGUMENT;
 *logits=0;m->inside_step=1;
 qw_profile*p=m->options.profile_clock?m->options.profile:0;uint64_t start=p?m->options.profile_clock(m->options.profile_clock_user):0;
 tensor_work*w=work(m);int rc=0;
 while(quota--&&t->state==QW_TENSOR_TASK_RUNNING){
  if(t->phase==QW_TENSOR_PHASE_EMBED){
   for(unsigned j=0;j<1024;j++)w->x[j]=value(m,m->embedding,t->token*1024+j);
   trace(m,UINT32_MAX,"embedding",w->x,1024);t->phase=QW_TENSOR_PHASE_LAYER;
  }else if(t->phase==QW_TENSOR_PHASE_LAYER){
   if(t->layer>=24){rc=QW_FORMAT;break;}rc=forward_layer(m,t->layer);if(rc)break;
   if(++t->layer==24)t->phase=QW_TENSOR_PHASE_FINAL_NORM;
  }else if(t->phase==QW_TENSOR_PHASE_FINAL_NORM){
   rc=norm(m,w->normal,w->x,m->final_norm,1024,1,4295,0);if(rc)break;
   trace(m,24,"final.norm",w->normal,1024);t->phase=t->emit?QW_TENSOR_PHASE_HEAD:QW_TENSOR_PHASE_COMMIT;
  }else if(t->phase==QW_TENSOR_PHASE_HEAD){
   if(!t->head_prepared){
    rc=matrix_prepare_activation(m,m->output_head,w->normal,&t->head_maximum);if(rc)break;
    t->head_prepared=1; /* maximum==0 is a valid prepared zero vector. */
   }
   uint32_t done=0;rc=matrix_execute_slice(m,m->output_head,(int32_t*)m->logits,1,t->head_row,4096u,t->head_maximum,&done);if(rc)break;
   t->head_row+=done;if(t->head_row==m->output_head->n){
    if(m->options.trace)m->options.trace(m->options.trace_user,m->position,24,"logits",m->logits,QW_VOCAB);
    t->phase=QW_TENSOR_PHASE_COMMIT;
   }
  }else if(t->phase==QW_TENSOR_PHASE_COMMIT){
   if(m->tensor_status.numeric.status&NPU_FIXED_STATUS_ERROR_MASK){rc=QW_NUMERIC;break;}
   m->position++;m->tensor_status.tokens++;m->logits_valid=!!t->emit;m->last_error=0;
   m->task_active=0;m->task_owner=0;t->head_prepared=0;t->head_maximum=0;t->state=QW_TENSOR_TASK_DONE;if(p)p->tokens_completed++;
   *logits=t->emit?m->logits:0;
  }else{rc=QW_FORMAT;break;}
  t->work_units++;
 }
 if(p)p->totalstep_ticks+=m->options.profile_clock(m->options.profile_clock_user)-start;
 m->inside_step=0;
 if(rc){m->needs_reset=1;m->last_error=rc;t->state=rc==QW_UNKNOWN?QW_TENSOR_TASK_UNKNOWN:QW_TENSOR_TASK_FAILED;if(rc==QW_UNKNOWN)m->tensor_status.unknown=1;else{m->task_active=0;m->task_owner=0;t->head_prepared=0;t->head_maximum=0;}if(p)p->tokens_failed++;return rc;}
 return t->state==QW_TENSOR_TASK_DONE?0:QW_PENDING;
}
int qw_tensor_task_discard(qw_tensor_task*t){
 if(!t||t->magic!=QW_TENSOR_TASK_MAGIC||!t->model||t->model->magic!=QW_MAGIC)return QW_ARGUMENT;
 qw_model*m=t->model;if(task_unknown(m))return QW_UNKNOWN;if(m->inside_step)return QW_BUSY;
 if(t->state==QW_TENSOR_TASK_DONE)return QW_ARGUMENT;
 if(m->task_active&&m->task_owner!=t)return QW_BUSY;
 if(t->state!=QW_TENSOR_TASK_RUNNING&&t->state!=QW_TENSOR_TASK_UNKNOWN&&t->state!=QW_TENSOR_TASK_FAILED)return QW_ARGUMENT;
 m->task_active=0;m->task_owner=0;m->needs_reset=1;m->logits_valid=0;t->head_prepared=0;t->head_maximum=0;t->state=QW_TENSOR_TASK_FAILED;return 0;
}
int qw_tensor_step(qw_model*m,uint32_t token,const float**logits,int emit){
 int rc=qw_tensor_task_begin(&m->sync_task,m,token,emit);if(rc)return rc;
 do{rc=qw_tensor_task_advance(&m->sync_task,1u,logits);}while(rc==QW_PENDING);return rc;
}
#endif
