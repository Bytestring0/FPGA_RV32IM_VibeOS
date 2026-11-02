#include "qw_internal.h"
#include "qw_math.h"
#include "qw_libm.h"
#include "vendor/npu_v1/npu_runtime.h"
#include <string.h>

/* FP32 SOFTWARE_REFERENCE scheduler, not an invented asynchronous NPU ABI. */
enum { S_EMBED=1,S_NORM,S_LQKV,S_LZ,S_LA,S_LB,S_CONV,S_DELTA,S_LOUT,
 S_AQ,S_AK,S_AV,S_KEYS,S_QUERY,S_SCORE,S_EXP,S_SCALE,S_VALUE,S_AOUT,
 S_RESID,S_FNORM,S_GATE,S_UP,S_SILU,S_DOWN,S_FRESID,S_FINAL,S_HEAD,S_COMMIT };
static void tr(qw_model *m,const char *s,const float *p,size_t n){
 if(m->options.trace)m->options.trace(m->options.trace_user,m->position,
 m->execution.stage==S_EMBED?UINT32_MAX:
 m->execution.stage>=S_FINAL?24u:m->execution.layer,s,p,n);
}
static void go(qw_model *m,unsigned stage){
 m->execution.stage=stage;m->execution.row=0;m->execution.index=0;
 m->execution.mat_ready=0;
}
static void rms(qw_model *m,float *out,const float *in,const qw_tensor *t,
                unsigned n,int centered,const char *tag){
 if(m->options.trace){float ss=0;tr(m,tag,in,n);
  for(unsigned j=0;j<n;j++)ss=ss+in[j]*in[j];tr(m,"norm.sumsq",&ss,1);}
 for(unsigned j=0;j<n;j++)m->w->norm_weight[j]=qw_value(t,j);
 qw_rms(out,in,m->w->norm_weight,n,centered);
}
static int mat(qw_model *m,const qw_tensor *t,const float *in,float *out,unsigned next){
 qw_execution *e=&m->execution;
 if(!t||t->dtype!=QT_W4||!t->k||t->k>4096)return QW_FORMAT;
 if(!e->mat_ready){
  memset(m->activation,0,m->plan.activation_bytes);
  if(npu_quantize_activation(in,t->k,m->activation,&e->sx))return QW_NUMERIC;
  e->mat_ready=1;return QW_PENDING;
 }
 uint32_t rows=t->n-e->row;if(rows>8)rows=8;
 if(rows>m->options.tile_rows)rows=m->options.tile_rows;
 npu_raw_job j={0};j.rows=rows;j.cols=t->k;j.weight_offset=t->ddr+e->row*t->stride;
 j.activation_offset=m->grants[1].offset;j.partial_offset=m->grants[2].offset;
 j.weight_stride=t->stride;j.global_row_base=e->row;
 npu_raw_layout layout;if(npu_raw_query(&j,m->grants,3,&layout))return QW_FORMAT;
 const npu_raw_result_view *v=0;
 int rc=npu_raw_execute(&m->raw,&j,m->options.budgets,&v);
 if(rc==NPU_RAW_UNKNOWN)return QW_UNKNOWN;if(rc)return QW_DEVICE;
 if(!v||!v->valid||v->rows!=rows||v->groups!=layout.groups||
    v->stride!=layout.output_stride||v->physical_ddr_offset!=j.partial_offset||
    v->global_row_base!=e->row)return QW_DEVICE;
 if(qw_reduce(m->partial,layout.partial_span,rows,layout.groups,v->stride,
              t->scales+(size_t)e->row*layout.groups,e->sx,out+e->row))return QW_NUMERIC;
 m->npu_jobs++;e->row+=rows;
 if(e->row==t->n)go(m,next);
 return QW_PENDING;
}
static int unit(qw_model *m){
 qw_execution *e=&m->execution;qw_work *w=m->w;
 qw_layer *l=&m->layers[e->layer<QW_LAYERS?e->layer:0];
 unsigned index=e->layer-e->layer/4,end;float *keys,*values;
 switch(e->stage){
 case S_EMBED:
  for(unsigned j=0;j<1024;j++)w->x[j]=qw_value(m->embedding,e->tokens[e->done]*1024+j);
  e->layer=0;tr(m,"embedding",w->x,1024);go(m,S_NORM);break;
 case S_NORM:
  memcpy(w->residual,w->x,sizeof w->x);
  rms(m,w->normal,w->x,l->norm,1024,1,"norm.attn.input");e->head=0;
  go(m,e->layer%4==3?S_AQ:S_LQKV);break;
 case S_LQKV:return mat(m,l->qkv,w->normal,w->qkv,S_LZ);
 case S_LZ:return mat(m,l->z,w->normal,w->z,S_LA);
 case S_LA:return mat(m,l->a,w->normal,w->a,S_LB);
 case S_LB:return mat(m,l->b,w->normal,w->b,S_CONV);
 case S_CONV:{
  if(!e->index){tr(m,"linear.qkv",w->qkv,6144);
   tr(m,"linear.conv.state.before",m->conv+(size_t)index*6144*3,6144*3);}
  float *conv=m->conv+(size_t)index*6144*3;
  end=e->index+128;if(end>6144)end=6144;
  for(unsigned c=e->index;c<end;c++){
   float *s=conv+c*3,v=0;
   for(unsigned j=0;j<3;j++)v=v+s[j]*qw_value(l->conv,c*4+j);
   v=v+w->qkv[c]*qw_value(l->conv,c*4+3);
   s[0]=s[1];s[1]=s[2];s[2]=w->qkv[c];w->qkv[c]=qw_silu(v);
  }
  e->index=end;if(end==6144){tr(m,"linear.conv",w->qkv,6144);
   tr(m,"linear.conv.state.after",conv,6144*3);e->head=0;go(m,S_DELTA);}break;
 }
 case S_DELTA:{
  unsigned h=e->head;float a=w->a[h]+qw_value(l->dt,h);
  float softplus=a>20?a:log1pf(expf(a));
  float decay=expf(-expf(qw_value(l->al,h))*softplus);
  float beta=qw_sigmoid(w->b[h]);
  float *state=m->delta+((size_t)index*16+h)*128*128,*out=w->attention+h*128;
  tr(m,"linear.delta.decay",&decay,1);tr(m,"linear.delta.beta",&beta,1);
  tr(m,"linear.delta.q",w->qkv+h*128,128);
  tr(m,"linear.delta.k",w->qkv+2048+h*128,128);
  tr(m,"linear.delta.v",w->qkv+4096+h*128,128);
  tr(m,"linear.delta.state.before",state,128*128);
  qw_delta(state,w->qkv+h*128,w->qkv+2048+h*128,w->qkv+4096+h*128,
           decay,beta,out,128);
  tr(m,"linear.delta.state.after",state,128*128);
  rms(m,out,out,l->lnorm,128,0,"norm.delta.input");
  for(unsigned j=0;j<128;j++)out[j]*=qw_silu(w->z[h*128+j]);
  if(++e->head==16){tr(m,"linear.gated",w->attention,2048);go(m,S_LOUT);}break;
 }
 case S_LOUT:return mat(m,l->out,w->attention,w->out,S_RESID);
 case S_AQ:return mat(m,l->q,w->normal,w->qkv,S_AK);
 case S_AK:return mat(m,l->k,w->normal,w->qkv+4096,S_AV);
 case S_AV:return mat(m,l->v,w->normal,w->qkv+4608,S_KEYS);
 case S_KEYS:{
  keys=m->kv+(size_t)(e->layer/4)*m->options.context_tokens*1024;
  values=keys+(size_t)m->options.context_tokens*512;
  unsigned h=e->head;float *k=w->qkv+4096+h*256;
  rms(m,k,k,l->kn,256,1,"norm.key.input");qw_rope(k,256,64,m->position,10000000.0f);
  memcpy(keys+(size_t)m->position*512+h*256,k,256*sizeof(float));
  memcpy(values+(size_t)m->position*512+h*256,w->qkv+4608+h*256,256*sizeof(float));
  tr(m,"attention.key.state",k,256);tr(m,"attention.value.state",w->qkv+4608+h*256,256);
  if(++e->head==2){e->head=0;go(m,S_QUERY);}break;
 }
 case S_QUERY:{
  float *q=w->qkv+e->head*512;
  rms(m,q,q,l->qn,256,1,"norm.query.input");qw_rope(q,256,64,m->position,10000000.0f);
  e->maximum=-INFINITY;e->total=0;go(m,S_SCORE);break;
 }
 case S_SCORE:
  keys=m->kv+(size_t)(e->layer/4)*m->options.context_tokens*1024;
  end=e->index+32;if(end>m->position+1)end=m->position+1;
  for(unsigned p=e->index;p<end;p++){
   const float *k=keys+(size_t)p*512+(e->head/4)*256;float sum=0;
   for(unsigned j=0;j<256;j++)sum=sum+w->qkv[e->head*512+j]*k[j];
   sum*=0.0625f;w->scores[p]=sum;if(sum>e->maximum)e->maximum=sum;
  }
  e->index=end;if(end==m->position+1)go(m,S_EXP);break;
 case S_EXP:
  end=e->index+32;if(end>m->position+1)end=m->position+1;
  for(unsigned p=e->index;p<end;p++){w->scores[p]=expf(w->scores[p]-e->maximum);e->total+=w->scores[p];}
  e->index=end;if(end==m->position+1){if(!qw_finite(e->total)||e->total<=0)return QW_NUMERIC;go(m,S_SCALE);}break;
 case S_SCALE:
  end=e->index+32;if(end>m->position+1)end=m->position+1;
  for(unsigned p=e->index;p<end;p++)w->scores[p]/=e->total;
  e->index=end;if(end==m->position+1)go(m,S_VALUE);break;
 case S_VALUE:
  keys=m->kv+(size_t)(e->layer/4)*m->options.context_tokens*1024;
  values=keys+(size_t)m->options.context_tokens*512;
  end=e->index+8;if(end>256)end=256;
  for(unsigned j=e->index;j<end;j++){
   float sum=0;for(unsigned p=0;p<=m->position;p++)sum=sum+w->scores[p]*values[(size_t)p*512+(e->head/4)*256+j];
   w->attention[e->head*256+j]=sum*qw_sigmoid(w->qkv[e->head*512+256+j]);
  }
  e->index=end;if(end==256){if(++e->head<8)go(m,S_QUERY);
   else{tr(m,"attention.gated",w->attention,2048);go(m,S_AOUT);}}break;
 case S_AOUT:return mat(m,l->out,w->attention,w->out,S_RESID);
 case S_RESID:
  tr(m,"residual.attn.left",w->residual,1024);tr(m,"residual.attn.right",w->out,1024);
  for(unsigned j=0;j<1024;j++)w->x[j]=w->residual[j]+w->out[j];
  go(m,S_FNORM);break;
 case S_FNORM:rms(m,w->normal,w->x,l->post,1024,1,"norm.ffn.input");go(m,S_GATE);break;
 case S_GATE:return mat(m,l->gate,w->normal,w->gate,S_UP);
 case S_UP:return mat(m,l->up,w->normal,w->up,S_SILU);
 case S_SILU:
  for(unsigned j=0;j<3584;j++)w->gate[j]=qw_silu(w->gate[j])*w->up[j];go(m,S_DOWN);break;
 case S_DOWN:return mat(m,l->down,w->gate,w->out,S_FRESID);
 case S_FRESID:
  tr(m,"residual.ffn.left",w->x,1024);tr(m,"residual.ffn.right",w->out,1024);
  for(unsigned j=0;j<1024;j++){w->x[j]+=w->out[j];if(!qw_finite(w->x[j]))return QW_NUMERIC;}
  tr(m,"layer.output",w->x,1024);
  if(++e->layer<QW_LAYERS)go(m,S_NORM);
  else go(m,e->want_logits&&e->done+1==e->count?S_FINAL:S_COMMIT);break;
 case S_FINAL:
  rms(m,w->normal,w->x,m->final_norm,1024,1,"norm.final.input");tr(m,"final.norm",w->normal,1024);go(m,S_HEAD);break;
 case S_HEAD:
  end=e->row+64;if(end>QW_VOCAB)end=QW_VOCAB;
  for(unsigned r=e->row;r<end;r++){
   float sum=0;for(unsigned j=0;j<1024;j++)sum=sum+qw_value(m->embedding,r*1024+j)*w->normal[j];
   if(!qw_finite(sum))return QW_NUMERIC;m->logits[r]=sum;
  }
  e->row=end;if(end==QW_VOCAB){tr(m,"logits",m->logits,QW_VOCAB);m->logits_valid=1;go(m,S_COMMIT);}break;
 case S_COMMIT:
  m->processed_ids[m->position++]=e->tokens[e->done++];
  if(e->done==e->count){e->active=0;e->tokens=0;return QW_OK;}
  m->logits_valid=0;go(m,S_EMBED);break;
 default:return QW_FORMAT;
 }
 return QW_PENDING;
}
int qw_start(qw_model *m,const uint32_t *tokens,size_t count,int want_logits){
 if(!m||m->magic!=QW_MAGIC||!tokens||!count||(want_logits!=0&&want_logits!=1))return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;if(m->checkpoint_active)return QW_BUSY;if(m->execution.active)return QW_BUSY;
 if(m->needs_reset)return QW_NEED_RESET;
 if(m->options.mode!=NPU_RAW_SOFTWARE_REFERENCE)return QW_UNSUPPORTED;
 if(m->position>m->options.context_tokens||count>m->options.context_tokens-m->position)return QW_CONTEXT_FULL;
 for(size_t i=0;i<count;i++)if(tokens[i]>=QW_VOCAB)return QW_TOKEN;
 memset(&m->execution,0,sizeof m->execution);
 m->execution.tokens=tokens;m->execution.count=count;m->execution.want_logits=(uint32_t)want_logits;
 m->execution.active=1;m->logits_valid=0;m->last_error=0;go(m,S_EMBED);return QW_OK;
}
int qw_poll(qw_model *m,uint32_t quota,uint32_t budget_ms,size_t *done,const float **logits){
 if(done)*done=0;if(logits)*logits=0;
 if(!m||m->magic!=QW_MAGIC||!done||!logits||!quota||(budget_ms&&!m->options.now_ms))return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;if(m->checkpoint_active)return QW_BUSY;if(!m->execution.active)return QW_ARGUMENT;
 qw_execution *e=&m->execution;uint64_t begin=m->options.now_ms?m->options.now_ms(m->options.clock_user):0;
 e->last_poll_units=0;int rc=QW_PENDING;
 while(e->last_poll_units<quota){
  rc=unit(m);e->last_poll_units++;
  if(rc!=QW_PENDING)break;
  if(budget_ms){uint64_t now=m->options.now_ms(m->options.clock_user);if(now<begin||now-begin>=budget_ms)break;}
 }
 e->last_poll_ms=m->options.now_ms?m->options.now_ms(m->options.clock_user)-begin:0;
 if(e->last_poll_ms>e->max_poll_ms)e->max_poll_ms=e->last_poll_ms;
 *done=e->done;
 if(rc<0){e->active=0;m->needs_reset=1;m->logits_valid=0;m->last_error=rc;}
 if(rc==QW_OK&&m->logits_valid)*logits=m->logits;
 return rc;
}
int qw_cancel(qw_model *m){
 if(!m||m->magic!=QW_MAGIC)return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;
 if(m->checkpoint_active)return QW_BUSY;
 if(m->execution.active){m->execution.active=0;m->execution.tokens=0;
  m->needs_reset=1;m->logits_valid=0;m->last_error=QW_CANCELLED;return QW_CANCELLED;}
 return QW_OK;
}
int qw_decode(qw_model *m,uint32_t token,const float **logits){
 if(m&&m->magic==QW_MAGIC&&m->options.mode==NPU_RAW_HARDWARE)return qw_decode_reference(m,token,logits);
 if(logits)*logits=0;if(!logits)return QW_ARGUMENT;
 int rc=qw_start(m,&token,1,1);if(rc)return rc;size_t done=0;
 do{rc=qw_poll(m,256,0,&done,logits);}while(rc==QW_PENDING);return rc;
}
