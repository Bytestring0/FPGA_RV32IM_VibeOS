#include "qw_internal.h"
#include "qw_int_head.h"
#include "npu_fixed_math.h"
#include <string.h>

#if NPU_FIXED_NUMERIC_VERSION != QW_INTEGER_HEAD_NUMERIC_VERSION
#error "Integer head requires the declared fixed-math numeric version"
#endif

static uint32_t add_count(uint32_t a,uint32_t b){return b>UINT32_MAX-a?UINT32_MAX:a+b;}
static void underflow(npu_fixed_status*s){s->status|=NPU_FIXED_STATUS_UNDERFLOW;s->underflows=add_count(s->underflows,1);}
static int numeric_failure(const npu_fixed_status*s){return (s->status&(NPU_FIXED_STATUS_ERROR_MASK|NPU_FIXED_STATUS_SATURATED))!=0;}
static int finish(qw_model*m,const npu_fixed_status*s,int rc){
 m->head_integer_saturations=add_count(m->head_integer_saturations,s->saturations);
 m->head_integer_underflows=add_count(m->head_integer_underflows,s->underflows);
 if(rc==QW_NUMERIC)m->head_integer_numeric_errors=add_count(m->head_integer_numeric_errors,1);
 return rc;
}
static qw_profile *profiling(const qw_model*m){return m->options.profile_clock?m->options.profile:0;}
static uint64_t now(const qw_model*m){return m->options.profile_clock(m->options.profile_clock_user);}
static uint32_t storage_word(const void*p,uint32_t i){uint32_t u;memcpy(&u,(const unsigned char*)p+(size_t)i*4,4);return u;}
static uint16_t le16(const unsigned char*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static int32_t le32_signed(const unsigned char*p){
 uint32_t u=p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
 return u<=INT32_MAX?(int32_t)u:-1-(int32_t)(UINT32_MAX-u);
}
static unsigned top_bit(uint32_t u){unsigned n=0;while(u>>1){u>>=1;n++;}return n;}
/* Every caller bounds nonnegative shifts so the result fits uint32. A right
 * shift >=32 rounds this <=24-bit significand to zero, without undefined C. */
static uint32_t shift_rne(uint32_t u,int shift){
 if(shift>=0)return u<<(unsigned)shift;
 unsigned right=(unsigned)-shift;if(right>=32)return 0;
 uint32_t whole=u>>right,mask=(UINT32_C(1)<<right)-1u;
 uint32_t tail=u&mask,half=UINT32_C(1)<<(right-1u);
 return whole+(uint32_t)(tail>half||(tail==half&&(whole&1u)));
}
static uint32_t magnitude_int16(uint32_t magnitude,int exponent){
 uint32_t field=magnitude>>23,mantissa=magnitude&UINT32_C(0x7fffff);
 int source_exponent=-149;if(field){mantissa|=UINT32_C(0x800000);source_exponent=(int)field-150;}
 return shift_rne(mantissa,source_exponent-exponent);
}
static int decode_input(const void*bits,int16_t*out,int*exponent,npu_fixed_status*s){
 uint32_t maximum=0;
 for(uint32_t i=0;i<QW_HIDDEN;i++){
  uint32_t magnitude=storage_word(bits,i)&UINT32_C(0x7fffffff);
  if(magnitude>=UINT32_C(0x7f800000)){
   s->status|=NPU_FIXED_STATUS_NONFINITE;s->invalid_inputs=add_count(s->invalid_inputs,1);return QW_NUMERIC;
  }
  if(magnitude>maximum)maximum=magnitude;
 }
 int common=0;
 if(maximum){
  uint32_t field=maximum>>23;
  int highest=field?(int)field-127:(int)top_bit(maximum)-149;
  common=highest-14;
  if(magnitude_int16(maximum,common)>32767u)common++;
 }
 for(uint32_t i=0;i<QW_HIDDEN;i++){
  uint32_t word=storage_word(bits,i),magnitude=word&UINT32_C(0x7fffffff);
  uint32_t value=magnitude_int16(magnitude,common);
  if(value>32767u){
   s->status|=NPU_FIXED_STATUS_SATURATED;s->saturations=add_count(s->saturations,1);return QW_NUMERIC;
  }
  if(magnitude&&!value)underflow(s);
  out[i]=(int16_t)((word>>31)?-(int32_t)value:(int32_t)value);
 }
 *exponent=common;return 0;
}
/* int32 Q16 is either zero or a normal binary32 number. No subnormal output
 * or overflow is possible; integer sign/magnitude also handles INT32_MIN. */
static uint32_t pack_q16(int32_t value){
 if(!value)return 0;
 uint32_t sign=value<0?UINT32_C(0x80000000):0;
 uint32_t magnitude=value<0?UINT32_C(0)-(uint32_t)value:(uint32_t)value;
 unsigned highest=top_bit(magnitude);
 uint32_t mantissa=shift_rne(magnitude,23-(int)highest);
 if(mantissa==UINT32_C(0x1000000)){mantissa>>=1;highest++;}
 return sign|((uint32_t)((int)highest-16+127)<<23)|(mantissa&UINT32_C(0x7fffff));
}
static int selected_head(const qw_model*m){
 const qw_tensor*t=m->output_head;
 return m->head_candidate_allowed&&t&&t==m->head_candidate&&t->dtype==QT_W4&&
  t->n==QW_VOCAB&&t->k==QW_HIDDEN&&t->scales;
}
int qw_enable_integer_w4_head(qw_model*m){
 if(!m||m->magic!=QW_MAGIC)return QW_ARGUMENT;
#ifdef QW_ENABLE_TENSOR_V2
  if(m->tensor_status.unknown||m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
  if(m->task_active||m->inside_step)return QW_BUSY;
#endif
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->needs_reset)return QW_NEED_RESET;
 #ifdef QW_ENABLE_TENSOR_V2
  if(m->task_active||m->inside_step)return QW_BUSY;
#endif
  if(!selected_head(m))return QW_FORMAT;
 if(!m->head_integer_enabled){m->head_integer_enabled=1;m->logits_valid=0;}
 return 0;
}
int qw_get_integer_w4_head(const qw_model*m,qw_integer_head_status*s){
 if(!m||m->magic!=QW_MAGIC||!s)return QW_ARGUMENT;
 *s=(qw_integer_head_status){m->head_integer_enabled,QW_INTEGER_HEAD_NUMERIC_VERSION,
  m->head_integer_numeric_errors,m->head_integer_saturations,m->head_integer_underflows};
 return 0;
}
static int reduce_tile(qw_model*m,const qw_tensor*t,uint32_t first,uint32_t rows,
 const npu_raw_layout*l,const npu_fixed_a8_scale*scale,void*out,npu_fixed_status*s){
 /* This head has eight groups. Arrays contain native integers, while raw
  * records/package scales are explicitly little-endian byte-decoded. */
 if(l->groups!=QW_HIDDEN/128u||l->record_bytes!=8u||l->row_tag_offset!=0u||
  l->group_tag_offset!=2u||l->partial_value_offset!=4u||l->output_stride<l->groups*8u||
  (uint64_t)(rows-1u)*l->output_stride+(uint64_t)l->groups*8u>l->partial_span)return QW_DEVICE;
 for(uint32_t r=0;r<rows;r++){
  int32_t partials[QW_HIDDEN/128u],logit=0;
  uint16_t scales[QW_HIDDEN/128u];
  for(uint32_t g=0;g<l->groups;g++){
   const unsigned char*p=m->partial+(size_t)r*l->output_stride+(size_t)g*8u;
   if(le16(p)!=r||le16(p+2)!=g)return QW_DEVICE;
   partials[g]=le32_signed(p+4);
   scales[g]=le16((const unsigned char*)t->scales+((size_t)(first+r)*l->groups+g)*2u);
   /* Preserve the positive finite scale contract of the W4 package/runtime. */
   if((scales[g]&UINT16_C(0x8000))||!(scales[g]&UINT16_C(0x7fff))||
    (scales[g]&UINT16_C(0x7c00))==UINT16_C(0x7c00)){
    s->status|=NPU_FIXED_STATUS_INVALID_ARGUMENT;s->invalid_inputs=add_count(s->invalid_inputs,1);return QW_NUMERIC;
   }
  }
  uint32_t flags=npu_fixed_reduce_fp16_q16(partials,scales,l->groups,scale,0,&logit,s);
  if((flags&(NPU_FIXED_STATUS_ERROR_MASK|NPU_FIXED_STATUS_SATURATED))||numeric_failure(s))return QW_NUMERIC;
  uint32_t bits=pack_q16(logit);memcpy((unsigned char*)out+(size_t)(first+r)*4u,&bits,4);
 }
 return 0;
}
int qw_integer_head_execute(qw_model*m,const void*input_bits,void*output_bits){
 if(!m||m->magic!=QW_MAGIC||!input_bits||!output_bits)return QW_ARGUMENT;
 /* Check ownership before reading input or touching activation/partial/logits. */
 if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->needs_reset)return QW_NEED_RESET;
 if(!m->head_integer_enabled||!selected_head(m)||!m->options.tile_rows)return QW_FORMAT;
 if(!m->activation||!m->partial||m->plan.activation_bytes<QW_HIDDEN)return QW_CAPACITY;
 const qw_tensor*t=m->output_head;uint32_t tile_rows=qw_partial_tile_rows(m,t);if(!tile_rows)return QW_CAPACITY;
 int16_t hidden[QW_HIDDEN];int exponent=0;
 npu_fixed_a8_scale scale={0};npu_fixed_status status={0};
 qw_profile*p=profiling(m);uint64_t start=0;
 m->logits_valid=0;
 if(p){p->matrix_calls++;start=now(m);}
 int rc=decode_input(input_bits,hidden,&exponent,&status);
 if(!rc){
  memset(m->activation,0,m->plan.activation_bytes);
  npu_fixed_quantize_a8(hidden,QW_HIDDEN,exponent,m->activation,&scale,&status);
  for(uint32_t i=0;i<QW_HIDDEN;i++)if(hidden[i]&&!m->activation[i])underflow(&status);
  if(numeric_failure(&status))rc=QW_NUMERIC;
 }
 if(p)p->quantize_ticks+=now(m)-start;
 if(rc)return finish(m,&status,rc);
 for(uint32_t first=0;first<t->n;){
  uint32_t rows=t->n-first;if(rows>tile_rows)rows=tile_rows;
  uint64_t weight=(uint64_t)t->ddr+(uint64_t)first*t->stride;
  if(weight>UINT32_MAX)return finish(m,&status,QW_FORMAT);
  npu_raw_job job={0};job.rows=rows;job.cols=t->k;job.weight_offset=(uint32_t)weight;
  job.activation_offset=m->grants[1].offset;job.partial_offset=m->grants[2].offset;
  job.weight_stride=t->stride;job.global_row_base=first;
  npu_raw_layout layout;
  if(npu_raw_query(&job,m->grants,3,&layout)||layout.activation_span>m->plan.activation_bytes||
   layout.partial_span>m->plan.partial_bytes)return finish(m,&status,QW_FORMAT);
  const npu_raw_result_view*view=0;
  if(p){p->jobs_started++;start=now(m);}
  rc=npu_raw_execute(&m->raw,&job,m->options.budgets,&view);
  if(p)p->rawexecute_ticks+=now(m)-start;
  if(rc==NPU_RAW_UNKNOWN||m->raw.active||m->raw.cache_unknown)return finish(m,&status,QW_UNKNOWN);
  if(rc)return finish(m,&status,QW_DEVICE);
  if(!view||!view->valid||view->rows!=rows||view->groups!=layout.groups||
   view->stride!=layout.output_stride||view->physical_ddr_offset!=job.partial_offset||
   view->global_row_base!=first||view->result_count!=layout.expected_result_count)return finish(m,&status,QW_DEVICE);
  if(p){
   p->jobs_completed++;
   if(m->raw.config.mode==NPU_RAW_HARDWARE){p->hardwarebusy_cycles+=(uint64_t)view->cycles;p->hardwarebusy_jobs++;}
   start=now(m);
  }
  rc=reduce_tile(m,t,first,rows,&layout,&scale,output_bits,&status);
  if(p)p->scale_reduce_ticks+=now(m)-start;
  if(rc)return finish(m,&status,rc);
  m->npu_jobs++;first+=rows;
 }
 return finish(m,&status,0);
}
