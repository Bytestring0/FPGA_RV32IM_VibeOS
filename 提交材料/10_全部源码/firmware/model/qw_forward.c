#include "qw_internal.h"
#include "qw_int_head.h"
#include "qw_int_sample.h"
#include "qw_math.h"
#include "vendor/npu_v1/npu_runtime.h"
#include "qw_libm.h"
#include <string.h>
#if defined(QW_TENSOR_ONLY_TARGET) && !defined(QW_ENABLE_TENSOR_V2)
#error "QW_TENSOR_ONLY_TARGET requires QW_ENABLE_TENSOR_V2"
#endif
static int valid(const qw_model*m){return m&&m->magic==QW_MAGIC;}
static qw_profile *profiling(const qw_model*m){return m->options.profile_clock?m->options.profile:0;}
#ifndef QW_TENSOR_ONLY_TARGET
static uint64_t profile_now(const qw_model*m){return m->options.profile_clock(m->options.profile_clock_user);}
#endif
void qw_profile_clear(qw_profile*p){if(p)memset(p,0,sizeof *p);}
int qw_get_profile(const qw_model*m,qw_profile*p){if(!valid(m)||!p||!profiling(m))return QW_ARGUMENT;*p=*m->options.profile;return 0;}
int qw_set_lut_nonlinear(qw_model*m,uint32_t enabled){
 if(!valid(m)||enabled>1u)return QW_ARGUMENT;
#ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.unknown)return QW_UNKNOWN;
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
#endif
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->needs_reset||m->position)return QW_NEED_RESET;
 if(!!m->options.lut_nonlinear!=enabled){m->options.lut_nonlinear=enabled;m->logits_valid=0;}
 return 0;
}
int qw_get_lut_status(const qw_model*m,qw_lut_status*s){
 if(!valid(m)||!s)return QW_ARGUMENT;
 *s=m->lut_status;s->enabled=!!m->options.lut_nonlinear;s->numeric_version=QW_LUT_NUMERIC_VERSION;return 0;
}
int qw_try_opt_tile(qw_options*o,uint64_t wb,uint64_t cb,uint64_t jb,size_t cap,qw_memory_plan*p){
 if(!o)return QW_ARGUMENT;qw_options candidate=*o;qw_memory_plan plan;
 candidate.tile_rows=QW_TILE_ROWS_OPTIMIZED;int rc=qw_plan(&candidate,wb,cb,jb,cap,&plan);
 if(rc)return rc;o->tile_rows=candidate.tile_rows;if(p)*p=plan;return 0;
}
int qw_set_tile_rows(qw_model*m,uint32_t rows){
 if(!valid(m))return QW_ARGUMENT;
#ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.unknown)return QW_UNKNOWN;
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
#endif
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->needs_reset)return QW_NEED_RESET;
 if(!rows||rows>65535u)return QW_ARGUMENT;
 if((uint64_t)rows*256u>m->plan.partial_bytes)return QW_CAPACITY;
 m->options.tile_rows=rows;return 0;
}
#ifndef QW_TENSOR_ONLY_TARGET
static void trace(qw_model*m,unsigned layer,const char*stage,const float*p,size_t n){if(m->options.trace)m->options.trace(m->options.trace_user,m->position,layer,stage,p,n);}
int qw_matrix(qw_model*m,const qw_tensor*t,const float*input,float*out){
 if(!t||t->dtype!=QT_W4||t->k>4096)return QW_FORMAT;
 qw_profile*p=profiling(m);uint64_t start=0;if(p){p->matrix_calls++;start=profile_now(m);}
 float sx;memset(m->activation,0,m->plan.activation_bytes);
 int rc=npu_quantize_activation(input,t->k,m->activation,&sx);
 if(p)p->quantize_ticks+=profile_now(m)-start;if(rc)return QW_NUMERIC;
 for(uint32_t first=0;first<t->n;){
  uint32_t rows=t->n-first;if(rows>m->options.tile_rows)rows=m->options.tile_rows;
#ifdef QW_PARTIAL_SRAM
  uint32_t capacity=qw_partial_tile_rows(m,t);if(rows>capacity)rows=capacity;
#endif
  npu_raw_job j={0};j.rows=rows;j.cols=t->k;j.weight_offset=t->ddr+first*t->stride;
  j.activation_offset=m->grants[1].offset;j.partial_offset=m->grants[2].offset;
  j.weight_stride=t->stride;j.global_row_base=first;npu_raw_layout l;
  if(npu_raw_query(&j,m->grants,3,&l))return QW_FORMAT;
  const npu_raw_result_view*v=0;if(p){p->jobs_started++;start=profile_now(m);}
  rc=npu_raw_execute(&m->raw,&j,m->options.budgets,&v);
  if(p)p->rawexecute_ticks+=profile_now(m)-start;
  if(rc==NPU_RAW_UNKNOWN)return QW_UNKNOWN;if(rc)return QW_DEVICE;
  if(!v||!v->valid||v->rows!=rows||v->groups!=l.groups||v->stride!=l.output_stride||v->physical_ddr_offset!=j.partial_offset||v->global_row_base!=first)return QW_DEVICE;
  if(p){p->jobs_completed++;if(m->options.mode==NPU_RAW_HARDWARE){p->hardwarebusy_cycles+=(uint64_t)v->cycles;p->hardwarebusy_jobs++;}start=profile_now(m);}
  rc=qw_reduce(m->partial,l.partial_span,rows,l.groups,v->stride,t->scales+(size_t)first*l.groups,sx,out+first);
  if(p)p->scale_reduce_ticks+=profile_now(m)-start;if(rc)return QW_NUMERIC;
  m->npu_jobs++;first+=rows;
 }return 0;
}
#else
int qw_matrix(qw_model*m,const qw_tensor*t,const float*input,float*out){
 (void)m;(void)t;(void)input;(void)out;return QW_FORMAT;
}
#endif
static qw_tensor *layer_tensor(qw_model*m,unsigned l,const char*suffix){
 char name[80];unsigned p=0;memcpy(name,"blk.",4);p=4;if(l>=10)name[p++]=(char)('0'+l/10);name[p++]=(char)('0'+l%10);name[p++]='.';
 size_t n=strlen(suffix);if(p+n>=sizeof name)return 0;memcpy(name+p,suffix,n+1);return qw_find(m,name);
}
static int vector(const qw_tensor*t,unsigned n){return t&&t->dtype!=QT_W4&&t->count==n;}
static int matrix(const qw_tensor*t,unsigned n,unsigned k){return t&&t->dtype==QT_W4&&t->n==n&&t->k==k;}
static int bind(qw_model*m){
 for(unsigned i=0;i<24;i++){
  qw_layer*l=&m->layers[i];
#define GET(member,name) l->member=layer_tensor(m,i,name)
  GET(norm,"attn_norm.weight");GET(post,"post_attention_norm.weight");GET(gate,"ffn_gate.weight");GET(up,"ffn_up.weight");GET(down,"ffn_down.weight");
  if(!vector(l->norm,1024)||!vector(l->post,1024)||!matrix(l->gate,3584,1024)||!matrix(l->up,3584,1024)||!matrix(l->down,1024,3584))return QW_FORMAT;
  if(i%4!=3){GET(qkv,"attn_qkv.weight");GET(z,"attn_gate.weight");GET(a,"ssm_alpha.weight");GET(b,"ssm_beta.weight");GET(conv,"ssm_conv1d.weight");GET(dt,"ssm_dt.bias");GET(al,"ssm_a");GET(lnorm,"ssm_norm.weight");GET(out,"ssm_out.weight");
   if(!matrix(l->qkv,6144,1024)||!matrix(l->z,2048,1024)||!matrix(l->a,16,1024)||!matrix(l->b,16,1024)||!vector(l->conv,6144*4)||!vector(l->dt,16)||!vector(l->al,16)||!vector(l->lnorm,128)||!matrix(l->out,1024,2048))return QW_FORMAT;
  }else{GET(q,"attn_q.weight");GET(k,"attn_k.weight");GET(v,"attn_v.weight");GET(qn,"attn_q_norm.weight");GET(kn,"attn_k_norm.weight");GET(out,"attn_output.weight");
   if(!matrix(l->q,4096,1024)||!matrix(l->k,512,1024)||!matrix(l->v,512,1024)||!vector(l->qn,256)||!vector(l->kn,256)||!matrix(l->out,1024,2048))return QW_FORMAT;
  }
#undef GET
 }return 0;
}
#ifndef QW_TENSOR_ONLY_TARGET
static void norm(qw_model*m,float*out,const float*in,const qw_tensor*t,unsigned n,int centered){
 for(unsigned i=0;i<n;i++)m->w->norm_weight[i]=qw_value(t,i);qw_rms(out,in,m->w->norm_weight,n,centered);
}
static int linear(qw_model*m,unsigned i){
 qw_layer*l=&m->layers[i];qw_work*w=m->w;int rc;
 if((rc=qw_matrix(m,l->qkv,w->normal,w->qkv))||(rc=qw_matrix(m,l->z,w->normal,w->z))||
 (rc=qw_matrix(m,l->a,w->normal,w->a))||(rc=qw_matrix(m,l->b,w->normal,w->b)))return rc;
 trace(m,i,"linear.qkv",w->qkv,6144);
 unsigned index=i-i/4;float*conv=m->conv+(size_t)index*6144*3;
 for(unsigned c=0;c<6144;c++){
  float *s=conv+c*3,v=0;
  for(unsigned j=0;j<3;j++)v=v+s[j]*qw_value(l->conv,c*4+j);
  v=v+w->qkv[c]*qw_value(l->conv,c*4+3);s[0]=s[1];s[1]=s[2];s[2]=w->qkv[c];
   if(m->options.lut_nonlinear){if((rc=qw_lut_silu(v,&w->qkv[c],&m->lut_status)))return rc;}
   else w->qkv[c]=qw_silu(v);
 }
 trace(m,i,"linear.conv",w->qkv,6144);
 for(unsigned h=0;h<16;h++){
  float a=w->a[h]+qw_value(l->dt,h);
  float softplus,beta;
   if(m->options.lut_nonlinear){
    if((rc=qw_lut_softplus(a,&softplus,&m->lut_status))||
       (rc=qw_lut_sigmoid(w->b[h],&beta,&m->lut_status)))return rc;
   }else{softplus=a>20?a:log1pf(expf(a));beta=qw_sigmoid(w->b[h]);}
   /* Composite decay remains FP pending independent range/state calibration. */
  float decay=expf(-expf(qw_value(l->al,h))*softplus);
  float *out=w->attention+h*128;
  qw_delta(m->delta+((size_t)index*16+h)*128*128,w->qkv+h*128,w->qkv+2048+h*128,w->qkv+4096+h*128,decay,beta,out,128);
  norm(m,out,out,l->lnorm,128,0);
  for(unsigned j=0;j<128;j++){
    float gate;if(m->options.lut_nonlinear){if((rc=qw_lut_silu(w->z[h*128+j],&gate,&m->lut_status)))return rc;}
    else gate=qw_silu(w->z[h*128+j]);out[j]*=gate;
   }
 }
 trace(m,i,"linear.gated",w->attention,2048);return qw_matrix(m,l->out,w->attention,w->out);
}
static int attention(qw_model*m,unsigned i){
 qw_layer*l=&m->layers[i];qw_work*w=m->w;int rc;
 if((rc=qw_matrix(m,l->q,w->normal,w->qkv))||(rc=qw_matrix(m,l->k,w->normal,w->qkv+4096))||(rc=qw_matrix(m,l->v,w->normal,w->qkv+4608)))return rc;
 float *keys=m->kv+(size_t)(i/4)*m->options.context_tokens*1024;
 float *values=keys+(size_t)m->options.context_tokens*512;
 for(unsigned h=0;h<2;h++){
  float*k=w->qkv+4096+h*256;norm(m,k,k,l->kn,256,1);qw_rope(k,256,64,m->position,10000000.0f);
  memcpy(keys+(size_t)m->position*512+h*256,k,256*sizeof(float));
  memcpy(values+(size_t)m->position*512+h*256,w->qkv+4608+h*256,256*sizeof(float));
 }
 for(unsigned h=0;h<8;h++){
  float*q=w->qkv+h*512;norm(m,q,q,l->qn,256,1);qw_rope(q,256,64,m->position,10000000.0f);
  float maximum=-INFINITY;
  for(unsigned pos=0;pos<=m->position;pos++){
   const float*k=keys+(size_t)pos*512+(h/4)*256;float sum=0;
   for(unsigned j=0;j<256;j++)sum=sum+q[j]*k[j];sum*=0.0625f;w->scores[pos]=sum;if(sum>maximum)maximum=sum;
  }
  float total=0;for(unsigned pos=0;pos<=m->position;pos++){
    float shifted=w->scores[pos]-maximum;
    if(m->options.lut_nonlinear){if((rc=qw_lut_exp_negative(shifted,&w->scores[pos],&m->lut_status)))return rc;}
    else w->scores[pos]=expf(shifted);total+=w->scores[pos];
   }
  if(!qw_finite(total)||total<=0)return QW_NUMERIC;
  for(unsigned pos=0;pos<=m->position;pos++)w->scores[pos]/=total;
  for(unsigned j=0;j<256;j++){
   float sum=0;for(unsigned pos=0;pos<=m->position;pos++)sum=sum+w->scores[pos]*values[(size_t)pos*512+(h/4)*256+j];
   float gate;if(m->options.lut_nonlinear){if((rc=qw_lut_sigmoid(q[256+j],&gate,&m->lut_status)))return rc;}
    else gate=qw_sigmoid(q[256+j]);w->attention[h*256+j]=sum*gate;
  }
 }
 trace(m,i,"attention.gated",w->attention,2048);return qw_matrix(m,l->out,w->attention,w->out);
}
#endif
static int qw_step(qw_model*m,uint32_t token,const float**logits,int emit_logits){
 if(logits)*logits=0;if(!valid(m)||!logits)return QW_ARGUMENT;
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
#ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.unknown)return QW_UNKNOWN;if(m->task_active||m->inside_step)return QW_BUSY;
#endif
 if(m->needs_reset)return QW_NEED_RESET;
 if(token>=QW_VOCAB)return QW_TOKEN;if(m->position>=m->options.context_tokens)return QW_CONTEXT_FULL;
#ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.enabled){
#ifdef QW_TENSOR_ONLY_TARGET
  if(!m->tensor_execute||m->raw.config.mode!=NPU_RAW_HARDWARE)return QW_FORMAT;
#endif
  return qw_tensor_step(m,token,logits,emit_logits);
 }
#endif
#ifdef QW_TENSOR_ONLY_TARGET
 /* Explicit hardware target: configuration failure never falls back to the
  * CPU FP graph. Host/reference builds retain the full compatibility path. */
 return QW_FORMAT;
#else
 qw_profile*p=profiling(m);uint64_t step_start=0;if(p){p->tokens_started++;step_start=profile_now(m);}
 m->logits_valid=0;qw_work*w=m->w;int rc=0;
 if(m->embedding_lazy){
  /* Tied embedding was not streamed at open; fetch exactly this row. */
  unsigned char*row=(unsigned char*)(uintptr_t)m->embedding->data+(uint64_t)token*(uint64_t)m->embedding_row_bytes;
  if(!m->options.embedding_read||
     m->options.embedding_read(m->options.embedding_user,
       m->embedding_source_offset+(uint64_t)token*(uint64_t)m->embedding_row_bytes,
       row,m->embedding_row_bytes)){rc=QW_IO;goto error;}
 }
 for(unsigned j=0;j<1024;j++)w->x[j]=qw_value(m->embedding,token*1024+j);
 trace(m,UINT32_MAX,"embedding",w->x,1024);
 for(unsigned i=0;i<24;i++){
  qw_layer*l=&m->layers[i];memcpy(w->residual,w->x,sizeof w->x);norm(m,w->normal,w->x,l->norm,1024,1);
  if((rc=i%4==3?attention(m,i):linear(m,i)))goto error;
  for(unsigned j=0;j<1024;j++)w->x[j]=w->residual[j]+w->out[j];
  trace(m,i,"residual.attn",w->x,1024);norm(m,w->normal,w->x,l->post,1024,1);trace(m,i,"ffn.norm",w->normal,1024);
  if((rc=qw_matrix(m,l->gate,w->normal,w->gate))||(rc=qw_matrix(m,l->up,w->normal,w->up)))goto error;
  trace(m,i,"ffn.gate",w->gate,3584);trace(m,i,"ffn.up",w->up,3584);
  for(unsigned j=0;j<3584;j++){
    float gate;if(m->options.lut_nonlinear){if((rc=qw_lut_silu(w->gate[j],&gate,&m->lut_status)))goto error;}
    else gate=qw_silu(w->gate[j]);w->gate[j]=gate*w->up[j];
   }
  trace(m,i,"ffn.product",w->gate,3584);if((rc=qw_matrix(m,l->down,w->gate,w->out)))goto error;trace(m,i,"ffn.down",w->out,1024);
  for(unsigned j=0;j<1024;j++){w->x[j]+=w->out[j];if(!qw_finite(w->x[j])){rc=QW_NUMERIC;goto error;}}
  trace(m,i,"layer.output",w->x,1024);
 }
 norm(m,w->normal,w->x,m->final_norm,1024,1);trace(m,24,"final.norm",w->normal,1024);
 /* Without an explicitly selected W4 candidate, use the original tied BF16
  * head. The embedding stays BF16 in either case. */
 if(emit_logits){
 uint64_t head_start=0;int bf16_head=!m->output_head;
 if(m->output_head){
  if(m->head_integer_enabled)rc=qw_integer_head_execute(m,w->normal,m->logits);
   else rc=qw_matrix(m,m->output_head,w->normal,m->logits);
   if(rc)goto error;
 }else{
 if(p){p->bf16head_calls++;head_start=profile_now(m);}
 if(m->embedding_lazy){rc=QW_UNKNOWN;goto error;} /* full-matrix head needs the whole matrix */
 if(m->options.bf16_head_kernel){
  if(m->options.bf16_head_kernel(m->options.bf16_head_user,m->embedding->data,w->normal,QW_VOCAB,1024,m->logits)){
   if(p)p->bf16head_ticks+=profile_now(m)-head_start;rc=QW_NUMERIC;goto error;
  }
 }else{
#if defined(QW_HOST_OPENMP)
#pragma omp parallel for
#endif
 for(uint32_t row=0;row<QW_VOCAB;row++){
  float sum=0;for(unsigned j=0;j<1024;j++)sum=sum+qw_value(m->embedding,row*1024+j)*w->normal[j];m->logits[row]=sum;
 }
 }
 }
 for(unsigned row=0;row<QW_VOCAB;row++)if(!qw_finite(m->logits[row])){if(p&&bf16_head)p->bf16head_ticks+=profile_now(m)-head_start;rc=QW_NUMERIC;goto error;}
 if(p&&bf16_head)p->bf16head_ticks+=profile_now(m)-head_start;
 trace(m,24,"logits",m->logits,QW_VOCAB);m->logits_valid=1;*logits=m->logits;
 }else{m->logits_valid=0;*logits=0;}
 m->position++;m->last_error=0;if(p){p->totalstep_ticks+=profile_now(m)-step_start;p->tokens_completed++;}return 0;
error:m->needs_reset=1;m->last_error=rc;if(p){p->totalstep_ticks+=profile_now(m)-step_start;p->tokens_failed++;}return rc;
#endif
}
int qw_decode(qw_model*m,uint32_t token,const float**logits){return qw_step(m,token,logits,1);}
int qw_prefill(qw_model*m,const uint32_t*tokens,size_t n,size_t*processed){
 if(processed)*processed=0;if(!valid(m)||!tokens||!n||!processed)return QW_ARGUMENT;
 #ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.unknown||m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
#endif
 if(n>m->options.context_tokens-m->position)return QW_CONTEXT_FULL;
 for(size_t i=0;i<n;i++)if(tokens[i]>=QW_VOCAB)return QW_TOKEN;
 for(size_t at=0;at<n;){size_t chunk=n-at;if(chunk>m->options.prefill_chunk_tokens)chunk=m->options.prefill_chunk_tokens;
  for(size_t j=0;j<chunk;j++){const float*out;int rc=qw_step(m,tokens[at+j],&out,(at+j+1==n)||m->options.trace!=0);if(rc)return rc;(*processed)++;}at+=chunk;
 }return 0;
}
int qw_get_status(const qw_model*m,qw_status*s){if(!valid(m)||!s)return QW_ARGUMENT;*s=(qw_status){m->position,m->needs_reset,m->raw.active||m->raw.cache_unknown,m->npu_jobs,m->last_error,m->logits_valid?m->logits:0,m->logits_valid?QW_VOCAB:0};
#ifdef QW_ENABLE_TENSOR_V2
 s->lease_active|=m->tensor_status.unknown;
#endif
 return 0;}
int qw_reset(qw_model*m){
 if(!valid(m))return QW_ARGUMENT;
#ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.unknown)return QW_UNKNOWN;
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
#endif
 if(m->raw.active&&npu_raw_recover(&m->raw,m->options.budgets.cache_polls))return QW_UNKNOWN;
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;int rc=bind(m);if(rc)return rc;
 memset(m->arena+m->plan.state_offset,0,m->plan.state_bytes);memset(m->w,0,sizeof *m->w);
 m->position=0;m->needs_reset=0;m->logits_valid=0;m->last_error=0;return 0;
}
int qw_close(qw_model*m){if(!valid(m))return QW_ARGUMENT;
#ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.unknown)return QW_UNKNOWN;
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
#endif
 if(m->raw.active&&npu_raw_recover(&m->raw,m->options.budgets.cache_polls))return QW_UNKNOWN;if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;m->magic=0;return 0;}
int qw_is_stop(uint32_t t){return t==248044u||t==248046u;}
int qw_sample(const float*l,size_t n,float temperature,uint32_t top_k,uint32_t*seed,uint32_t*out){
 uint32_t temperature_bits;memcpy(&temperature_bits,&temperature,sizeof temperature_bits);
  if(!l||!n||n>QW_VOCAB||!seed||!out||
     (temperature_bits&UINT32_C(0x7f800000))==UINT32_C(0x7f800000)||
     ((temperature_bits&UINT32_C(0x80000000))&&(temperature_bits&UINT32_C(0x7fffffff))))return QW_ARGUMENT;
 if(!(temperature_bits&UINT32_C(0x7fffffff))||top_k==1)return qw_sample_greedy_bits(l,n,out);
  float max=-INFINITY;for(uint32_t i=0;i<n;i++){if(!qw_finite(l[i]))return QW_NUMERIC;if(l[i]>max)max=l[i];}

 if(!top_k||top_k>64)return QW_ARGUMENT;if(top_k>n)top_k=(uint32_t)n;
 float scores[64];uint32_t ids[64];unsigned count=0;
 for(uint32_t i=0;i<n;i++){
  unsigned pos=count;while(pos&&scores[pos-1]<l[i])pos--;
  if(pos>=top_k)continue;unsigned end=count<top_k?count:top_k-1;while(end>pos){scores[end]=scores[end-1];ids[end]=ids[end-1];end--;}
  scores[pos]=l[i];ids[pos]=i;if(count<top_k)count++;
 }
 float total=0;for(unsigned i=0;i<count;i++){scores[i]=expf((scores[i]-max)/temperature);total+=scores[i];}
 uint32_t s=*seed;if(!s)s=1;s^=s<<13;s^=s>>17;s^=s<<5;*seed=s;float pick=((float)(s>>8)/16777216.0f)*total;
 for(unsigned i=0;i<count;i++){if(pick<scores[i]){*out=ids[i];return 0;}pick-=scores[i];}*out=ids[count-1];return 0;
}
