#include "qw_internal.h"
#include "qw_head.h"
#include "qw_json.h"
#include "qw_math.h"
#include "qw_sha256.h"
#include "vendor/npu_v1/npu_ag_platform.h"
#include <string.h>
static int take(size_t *at,size_t n,size_t *where){if(*at>SIZE_MAX-31)return -1;*at=(*at+31)&~(size_t)31;if(n>SIZE_MAX-*at)return -1;*where=*at;*at+=n;return 0;}
int qw_plan(const qw_options*o,uint64_t wb,uint64_t cb,uint64_t jb,size_t cap,qw_memory_plan*p){
 if(!o||!p||!o->context_tokens||o->context_tokens>QW_CONTEXT_MAX||!o->prefill_chunk_tokens||o->prefill_chunk_tokens>128||!o->tile_rows||o->tile_rows>65535||o->arena_ddr_offset%32||
 o->arena_ddr_offset>=NPU_RAW_DDR_LIMIT||cap>NPU_RAW_DDR_LIMIT-o->arena_ddr_offset||
 (uint64_t)o->system_reserved_bytes+o->framebuffer_reserved_bytes>o->arena_ddr_offset||
 wb>UINT32_MAX||cb>UINT32_MAX||jb>2u*1024*1024||!wb||!cb||!jb)return QW_ARGUMENT;
 qw_memory_plan q={0};size_t at=0;
 q.weights_bytes=(size_t)wb;q.cpu_bytes=(size_t)cb;q.catalog_bytes=(size_t)jb;
 q.state_bytes=(size_t)(18u*16u*128u*128u+18u*6144u*3u+6u*o->context_tokens*1024u)*sizeof(float);
 q.work_bytes=sizeof(qw_work);q.activation_bytes=4096;q.partial_bytes=(size_t)o->tile_rows*256;
#ifdef QW_PARTIAL_SRAM
 q.partial_bytes=65536u;
#endif
 q.logits_bytes=QW_VOCAB*sizeof(float);q.context_tokens=o->context_tokens;q.policy_version=QW_POLICY_VERSION;
 q.system_reserved_bytes=o->system_reserved_bytes;q.framebuffer_reserved_bytes=o->framebuffer_reserved_bytes;
 if(take(&at,sizeof(qw_model),&q.context_offset)||take(&at,q.weights_bytes,&q.weights_offset)||take(&at,q.cpu_bytes,&q.cpu_offset)||take(&at,q.catalog_bytes,&q.catalog_offset)||
 take(&at,q.state_bytes,&q.state_offset)||take(&at,q.work_bytes,&q.work_offset)||take(&at,q.activation_bytes,&q.activation_offset))return QW_CAPACITY;
#ifdef QW_PARTIAL_SRAM
 if(at>SIZE_MAX-65535u)return QW_CAPACITY;at=(at+65535u)&~(size_t)65535u;
#endif
 if(take(&at,q.partial_bytes,&q.partial_offset)||take(&at,q.logits_bytes,&q.logits_offset))return QW_CAPACITY;
 q.arena_bytes=at;if(at>cap)return QW_CAPACITY;*p=q;return 0;
}
float qw_value(const qw_tensor*t,uint32_t i){
 if(t->dtype==QT_BF16)return qw_bf16((uint16_t)(t->data[2*i]|((unsigned)t->data[2*i+1]<<8)));
 union{uint32_t u;float f;}v;const unsigned char*p=t->data+4u*i;v.u=p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);return v.f;
}
qw_tensor *qw_find(qw_model*m,const char*name){uint32_t id=npu_package_name_id(name);for(uint32_t i=0;i<m->tensor_count;i++)if(m->tensors[i].id==id)return &m->tensors[i];return 0;}
static int field_u(qj_doc d,qj_value v,const char*k,uint64_t*x){qj_value a;return qj_get(d,v,k,&a)||qj_uint(d,a,x);}
static int head_shape(qj_doc d,qj_value v){
 qj_value a,dim;uint64_t n,k;if(qj_get(d,v,"source_shape",&a)||a.kind!='[')return -1;
 size_t at=a.begin+1;if(qj_read(d,&at,&dim)||qj_uint(d,dim,&n)||at>=a.end-1||d.s[at++]!=','||
 qj_read(d,&at,&dim)||qj_uint(d,dim,&k)||at!=a.end-1||n!=QW_VOCAB||k!=QW_HIDDEN)return -1;
 return 0;
}
int qw_bind_catalog(qw_model*m,const char*json,size_t n){
 qj_doc d={json,n};qj_value root,array;size_t at=0;
 if(qj_read(d,&at,&root)||at!=n||qj_get(d,root,"tensors",&array)||array.kind!='[')return QW_FORMAT;
 unsigned cpus=0,w4s=0;at=array.begin+1;
 while(at<array.end-1){qj_value v,a;char name[80],dtype[40];
  if(qj_read(d,&at,&v)||v.kind!='{'||qj_get(d,v,"name",&a)||qj_text(d,a,name,sizeof name)||qj_get(d,v,"target_dtype",&a)||qj_text(d,a,dtype,sizeof dtype)||m->tensor_count>=QW_MAX_TENSORS||qw_find(m,name))return QW_FORMAT;
  int is_head=!strcmp(name,QW_W4_HEAD_TENSOR_NAME);
  if(npu_package_name_id(name)==npu_package_name_id(QW_W4_HEAD_TENSOR_NAME)&&!is_head)return QW_FORMAT;
  if(is_head&&(!m->head_candidate_allowed||strcmp(dtype,"signed_W4_zero_point_0")||head_shape(d,v)))return QW_FORMAT;
  qw_tensor*t=&m->tensors[m->tensor_count];memset(t,0,sizeof *t);t->id=npu_package_name_id(name);
  if(!strcmp(dtype,"signed_W4_zero_point_0")){
   npu_package_tensor p;if(npu_package_find(&m->package,t->id,&p)||!p.k||p.n>UINT32_MAX/p.k)return QW_FORMAT;
   if(is_head&&(p.n!=QW_VOCAB||p.k!=QW_HIDDEN))return QW_FORMAT;
   t->n=p.n;t->k=p.k;t->count=p.n*p.k;t->dtype=QT_W4;t->stride=p.row_stride;
   t->data=m->package.buffer+(size_t)p.weight_offset;t->scales=(const uint16_t*)(m->package.buffer+(size_t)p.scale_offset);
   if(npu_package_ddr_offset(&m->package,m->options.arena_ddr_offset+(uint32_t)m->plan.weights_offset,p.weight_offset,p.weight_length,&t->ddr))return QW_FORMAT;
   if(is_head)m->head_candidate=t;
   w4s++;
  }else{
   t->dtype=!strcmp(dtype,"BF16")?QT_BF16:!strcmp(dtype,"F32")?QT_F32:0;if(!t->dtype)return QW_FORMAT;
   uint64_t offset,bytes;if(field_u(d,v,"source_raw_offset",&offset)||field_u(d,v,"source_raw_bytes",&bytes)||offset>m->plan.cpu_bytes||bytes>m->plan.cpu_bytes-offset||offset%4)return QW_FORMAT;
   if(qj_get(d,v,"source_shape",&a)||a.kind!='[')return QW_FORMAT;
   size_t sp=a.begin+1;uint64_t count=1;unsigned dimensions=0;
   while(sp<a.end-1){qj_value dim;uint64_t size;if(qj_read(d,&sp,&dim)||qj_uint(d,dim,&size)||!size||size>UINT32_MAX||count>UINT32_MAX/size||dimensions>=4)return QW_FORMAT;
    if(!dimensions)t->n=(uint32_t)size;count*=size;dimensions++;if(sp<a.end-1){if(d.s[sp++]!=',')return QW_FORMAT;}
   }
   if(!dimensions||count*(t->dtype==QT_BF16?2u:4u)!=bytes)return QW_FORMAT;
   t->count=(uint32_t)count;t->k=t->count/t->n;t->data=m->arena+m->plan.cpu_offset+(size_t)offset;cpus++;
   if(!strcmp(name,"token_embd.weight")){m->embedding_source_offset=offset;m->embedding_row_bytes=(uint32_t)t->k*(t->dtype==QT_BF16?2u:4u);}
  }
  m->tensor_count++;
  if(at<array.end-1){if(d.s[at++]!=',')return QW_FORMAT;}
 }
 unsigned expected_w4=QW_BASE_W4_TENSORS+(m->head_candidate_allowed?1u:0u);
 if(cpus!=QW_CPU_TENSORS||w4s!=expected_w4||m->tensor_count!=expected_w4+QW_CPU_TENSORS||
 m->package.tensor_count!=expected_w4||!!m->head_candidate!=!!m->head_candidate_allowed)return QW_FORMAT;
 m->embedding=qw_find(m,"token_embd.weight");m->final_norm=qw_find(m,"output_norm.weight");
 if(!m->embedding||m->embedding->dtype!=QT_BF16||m->embedding->n!=QW_VOCAB||m->embedding->k!=1024||!m->final_norm||m->final_norm->count!=1024)return QW_FORMAT;
 return 0;
}
#define LOAD_CHUNK_BYTES 65536u
typedef struct {
 npu_storage_source source;
 unsigned char *destination;
 uint64_t next;
} load_stream;
static int stream_init(load_stream *stream,const npu_storage_source *source,void *destination){
 if(!source||!source->read_exact_at||source->size_bytes>SIZE_MAX||
    (!destination&&source->size_bytes)||
    (source->size_bytes&&source->size_bytes-1u>UINTPTR_MAX-(uintptr_t)destination))return QW_ARGUMENT;
 stream->source=*source;stream->destination=destination;stream->next=0;return 0;
}
static int stream_read(void *ctx,uint64_t offset,void *destination,size_t bytes){
 load_stream *stream=ctx;
 if(offset!=stream->next||offset>stream->source.size_bytes||
    bytes>stream->source.size_bytes-offset||bytes>LOAD_CHUNK_BYTES||!bytes||
    destination!=stream->destination+(size_t)offset)return QW_IO;
 if(stream->source.read_exact_at(stream->source.ctx,offset,destination,bytes))return QW_IO;
 stream->next+=bytes;return 0;
}
static int stream_finish(load_stream *stream){
 while(stream->next<stream->source.size_bytes){
  uint64_t left=stream->source.size_bytes-stream->next;
  size_t n=left>LOAD_CHUNK_BYTES?LOAD_CHUNK_BYTES:(size_t)left;
  int rc=stream_read(stream,stream->next,stream->destination+(size_t)stream->next,n);if(rc)return rc;
 }
 return 0;
}
/* Stream [0,skip_begin) and [skip_end,size), leaving the skipped span untouched
 * in the arena. Used for the tied embedding matrix when lazy_embedding is on. */
static int load_source_skip(const npu_storage_source *source,void *destination,
 uint64_t skip_begin,uint64_t skip_end){
 load_stream stream;int rc;
 if(stream_init(&stream,source,destination))return QW_FORMAT;
 if(skip_begin>skip_end||skip_end>stream.source.size_bytes)return QW_FORMAT;
 while(stream.next<skip_begin){
  uint64_t left=skip_begin-stream.next;
  size_t n=left>LOAD_CHUNK_BYTES?LOAD_CHUNK_BYTES:(size_t)left;
  rc=stream_read(&stream,stream.next,stream.destination+(size_t)stream.next,n);if(rc)return rc;
 }
 stream.next=skip_end;
 return stream_finish(&stream);
}
static int load_source(const npu_storage_source *source,void *destination,const unsigned char *expected){
 load_stream stream;int rc;(void)expected;
 rc=stream_init(&stream,source,destination);return rc?rc:stream_finish(&stream);
}
static int load_weights(const npu_storage_source *source,void *destination,size_t capacity,
 const unsigned char *expected,npu_package_view *view){
 load_stream stream;npu_storage_source reader;int rc;(void)expected;
 if(!source||source->size_bytes>capacity)return QW_FORMAT;
 if(stream_init(&stream,source,destination))return QW_FORMAT;
 reader=(npu_storage_source){stream_read,&stream,stream.source.size_bytes};
 if(npu_package_load_from_source(&reader,0,reader.size_bytes,destination,capacity,view))return QW_FORMAT;
 rc=stream_finish(&stream);
 if(rc)*view=(npu_package_view){0};
 return rc;
}
static int open_model(qw_model**result,void*arena,size_t capacity,const qw_options*o,
 const npu_storage_source*weights,const npu_storage_source*cpu,const npu_storage_source*catalog,qw_memory_plan*plan,
 int allow_head,const char*head_name){
 if(!result)return QW_ARGUMENT;
 if(*result)return (*result)->magic==QW_MAGIC&&(*result)->raw.active?QW_UNKNOWN:QW_ARGUMENT;
 if(allow_head&&(!head_name||strcmp(head_name,QW_W4_HEAD_TENSOR_NAME)))return QW_ARGUMENT;
 if(!arena||((uintptr_t)arena&31)||!o||!weights||!cpu||!catalog)return QW_ARGUMENT;
 npu_storage_source snapshot[3]={*weights,*cpu,*catalog};
 weights=snapshot;cpu=snapshot+1;catalog=snapshot+2;
 qw_memory_plan p;int rc=qw_plan(o,weights->size_bytes,cpu->size_bytes,catalog->size_bytes,capacity,&p);if(rc)return rc;
 qw_model*m=(qw_model*)((unsigned char*)arena+p.context_offset);memset(m,0,sizeof *m);m->options=*o;m->plan=p;m->arena=arena;m->arena_capacity=capacity;
 m->head_candidate_allowed=allow_head?1u:0u;
 if((rc=load_weights(weights,m->arena+p.weights_offset,p.weights_bytes,o->weight_sha256,&m->package)))return rc;
 if((rc=load_source(catalog,m->arena+p.catalog_offset,o->catalog_sha256)))return rc;
 if((rc=qw_bind_catalog(m,(const char*)m->arena+p.catalog_offset,p.catalog_bytes)))return rc;
 if(o->lazy_embedding&&o->embedding_read&&m->embedding_row_bytes){
  uint64_t span=(uint64_t)m->embedding->count*(uint64_t)(m->embedding_row_bytes/m->embedding->k);
  m->embedding_lazy=1;
  rc=load_source_skip(cpu,m->arena+p.cpu_offset,m->embedding_source_offset,m->embedding_source_offset+span);
 }else rc=load_source(cpu,m->arena+p.cpu_offset,o->cpu_sha256);
 if(rc)return rc;
 m->w=(qw_work*)(m->arena+p.work_offset);m->delta=(float*)(m->arena+p.state_offset);
 m->conv=m->delta+18u*16u*128u*128u;m->kv=m->conv+18u*6144u*3u;
 m->activation=(int8_t*)(m->arena+p.activation_offset);m->partial=m->arena+p.partial_offset;m->logits=(float*)(m->arena+p.logits_offset);
 m->grants[0]=(npu_raw_region){o->arena_ddr_offset+(uint32_t)p.weights_offset,(uint32_t)p.weights_bytes,NPU_RAW_WEIGHT};
 m->grants[1]=(npu_raw_region){o->arena_ddr_offset+(uint32_t)p.activation_offset,(uint32_t)p.activation_bytes,NPU_RAW_ACTIVATION};
 m->grants[2]=(npu_raw_region){o->arena_ddr_offset+(uint32_t)p.partial_offset,(uint32_t)p.partial_bytes,NPU_RAW_PARTIAL};
 if(o->mode==NPU_RAW_HARDWARE)rc=npu_ag_open(&m->raw,o->device,m->grants,3);
 else if(o->mode==NPU_RAW_SOFTWARE_REFERENCE){npu_raw_config cfg={0};cfg.mode=o->mode;cfg.regions=m->grants;cfg.region_count=3;cfg.mapping=(npu_raw_mapping){m->arena,capacity,o->arena_ddr_offset};rc=npu_raw_init(&m->raw,&cfg);}else return QW_ARGUMENT;
 if(rc)return QW_DEVICE;m->magic=QW_MAGIC;if((rc=qw_reset(m)))return rc;
 if(plan)*plan=p;*result=m;return 0;
}
int qw_open(qw_model**result,void*arena,size_t capacity,const qw_options*o,
 const npu_storage_source*weights,const npu_storage_source*cpu,const npu_storage_source*catalog,qw_memory_plan*plan){
 return open_model(result,arena,capacity,o,weights,cpu,catalog,plan,0,0);
}
int qw_open_w4_head_candidate(qw_model**result,void*arena,size_t capacity,const qw_options*o,
 const npu_storage_source*weights,const npu_storage_source*cpu,const npu_storage_source*catalog,qw_memory_plan*plan,
 const char*exact_tensor_name){
 return open_model(result,arena,capacity,o,weights,cpu,catalog,plan,1,exact_tensor_name);
}
static int head_tiles_fit(const qw_model*m,const qw_tensor*t){
 uint32_t tile_rows=qw_partial_tile_rows(m,t);if(!tile_rows)return QW_FORMAT;
 uint32_t last=((t->n-1u)/tile_rows)*tile_rows;
 for(unsigned tile=0;tile<2;tile++){
  uint32_t first=tile?last:0,rows=t->n-first;if(rows>m->options.tile_rows)rows=m->options.tile_rows;
#ifdef QW_PARTIAL_SRAM
  if(rows>tile_rows)rows=tile_rows;
#endif
  uint64_t weight=(uint64_t)t->ddr+(uint64_t)first*t->stride;if(weight>UINT32_MAX)return QW_FORMAT;
  npu_raw_job j={0};j.rows=rows;j.cols=t->k;j.weight_offset=(uint32_t)weight;j.weight_stride=t->stride;
  j.activation_offset=m->grants[1].offset;j.partial_offset=m->grants[2].offset;j.global_row_base=first;
  npu_raw_layout l;if(npu_raw_query(&j,m->grants,3,&l)||l.partial_span>m->plan.partial_bytes||
  l.activation_span>m->plan.activation_bytes)return QW_FORMAT;
 }
 return 0;
}
int qw_enable_w4_head(qw_model*m,const char*exact_tensor_name){
 if(!m||m->magic!=QW_MAGIC)return QW_ARGUMENT;
 #ifdef QW_ENABLE_TENSOR_V2
  if(m->tensor_status.unknown)return QW_UNKNOWN;
#endif
  if(m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 #ifdef QW_ENABLE_TENSOR_V2
  if(m->task_active||m->inside_step)return QW_BUSY;
#endif
  if(m->needs_reset)return QW_NEED_RESET;
 if(!exact_tensor_name||strcmp(exact_tensor_name,QW_W4_HEAD_TENSOR_NAME))return QW_ARGUMENT;
 qw_tensor*t=m->head_candidate;
 if(!m->head_candidate_allowed||!t||t!=qw_find(m,exact_tensor_name)||t->dtype!=QT_W4||
 t->n!=QW_VOCAB||t->k!=QW_HIDDEN)return QW_FORMAT;
 int rc=head_tiles_fit(m,t);if(rc)return rc;
 if(m->output_head!=t){m->output_head=t;m->logits_valid=0;}
 return 0;
}
int qw_get_w4_head(const qw_model*m,const char**exact_tensor_name){
 if(!exact_tensor_name)return QW_ARGUMENT;*exact_tensor_name=0;
 if(!m||m->magic!=QW_MAGIC)return QW_ARGUMENT;
 if(m->output_head)*exact_tensor_name=QW_W4_HEAD_TENSOR_NAME;
 return 0;
}
