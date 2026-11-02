#include "qw_internal.h"
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
 q.logits_bytes=QW_VOCAB*sizeof(float);q.context_tokens=o->context_tokens;q.policy_version=QW_POLICY_VERSION;
 q.system_reserved_bytes=o->system_reserved_bytes;q.framebuffer_reserved_bytes=o->framebuffer_reserved_bytes;
 if(take(&at,sizeof(qw_model),&q.context_offset)||take(&at,q.weights_bytes,&q.weights_offset)||take(&at,q.cpu_bytes,&q.cpu_offset)||take(&at,q.catalog_bytes,&q.catalog_offset)||
 take(&at,q.state_bytes,&q.state_offset)||take(&at,q.work_bytes,&q.work_offset)||take(&at,q.activation_bytes,&q.activation_offset)||take(&at,q.partial_bytes,&q.partial_offset)||take(&at,q.logits_bytes,&q.logits_offset))return QW_CAPACITY;
 q.arena_bytes=at;if(at>cap)return QW_CAPACITY;*p=q;return 0;
}
float qw_value(const qw_tensor*t,uint32_t i){
 if(t->dtype==QT_BF16)return qw_bf16((uint16_t)(t->data[2*i]|((unsigned)t->data[2*i+1]<<8)));
 union{uint32_t u;float f;}v;const unsigned char*p=t->data+4u*i;v.u=p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);return v.f;
}
qw_tensor *qw_find(qw_model*m,const char*name){uint32_t id=npu_package_name_id(name);for(uint32_t i=0;i<m->tensor_count;i++)if(m->tensors[i].id==id)return &m->tensors[i];return 0;}
static int field_u(qj_doc d,qj_value v,const char*k,uint64_t*x){qj_value a;return qj_get(d,v,k,&a)||qj_uint(d,a,x);}
int qw_bind_catalog(qw_model*m,const char*json,size_t n){
 qj_doc d={json,n};qj_value root,array;size_t at=0;
 if(qj_read(d,&at,&root)||at!=n||qj_get(d,root,"tensors",&array)||array.kind!='[')return QW_FORMAT;
 unsigned cpus=0,w4s=0;at=array.begin+1;
 while(at<array.end-1){qj_value v,a;char name[80],dtype[40];
  if(qj_read(d,&at,&v)||v.kind!='{'||qj_get(d,v,"name",&a)||qj_text(d,a,name,sizeof name)||qj_get(d,v,"target_dtype",&a)||qj_text(d,a,dtype,sizeof dtype)||m->tensor_count>=320||qw_find(m,name))return QW_FORMAT;
  qw_tensor*t=&m->tensors[m->tensor_count];memset(t,0,sizeof *t);t->id=npu_package_name_id(name);
  if(!strcmp(dtype,"signed_W4_zero_point_0")){
   npu_package_tensor p;if(npu_package_find(&m->package,t->id,&p))return QW_FORMAT;
   t->n=p.n;t->k=p.k;t->count=p.n*p.k;t->dtype=QT_W4;t->stride=p.row_stride;
   t->data=m->package.buffer+(size_t)p.weight_offset;t->scales=(const uint16_t*)(m->package.buffer+(size_t)p.scale_offset);
   if(npu_package_ddr_offset(&m->package,m->options.arena_ddr_offset+(uint32_t)m->plan.weights_offset,p.weight_offset,p.weight_length,&t->ddr))return QW_FORMAT;
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
  }
  m->tensor_count++;
  if(at<array.end-1){if(d.s[at++]!=',')return QW_FORMAT;}
 }
 if(cpus!=134||w4s!=186||m->tensor_count!=320)return QW_FORMAT;
 m->embedding=qw_find(m,"token_embd.weight");m->final_norm=qw_find(m,"output_norm.weight");
 if(!m->embedding||m->embedding->dtype!=QT_BF16||m->embedding->n!=QW_VOCAB||m->embedding->k!=1024||!m->final_norm||m->final_norm->count!=1024)return QW_FORMAT;
 return 0;
}
static int hash_matches(const void*p,size_t n,const unsigned char*expected){
 if(!expected)return QW_ARGUMENT;qw_sha256 s;unsigned char digest[32];qw_sha_init(&s);qw_sha_update(&s,p,n);qw_sha_final(&s,digest);return memcmp(digest,expected,32)?QW_HASH:0;
}
static int load_source(const npu_storage_source*s,void*destination){
 if(!s||!s->read_exact_at||s->size_bytes>SIZE_MAX)return QW_ARGUMENT;
 unsigned char*p=destination;uint64_t off=0;while(off<s->size_bytes){size_t n=(size_t)(s->size_bytes-off);if(n>65536)n=65536;if(s->read_exact_at(s->ctx,off,p+(size_t)off,n))return QW_IO;off+=n;}return 0;
}
int qw_open(qw_model**result,void*arena,size_t capacity,const qw_options*o,
 const npu_storage_source*weights,const npu_storage_source*cpu,const npu_storage_source*catalog,qw_memory_plan*plan){
 if(!result)return QW_ARGUMENT;
 if(*result)return (*result)->magic==QW_MAGIC&&(*result)->raw.active?QW_UNKNOWN:QW_ARGUMENT;
 if(!arena||((uintptr_t)arena&31)||!o||!weights||!cpu||!catalog||!o->weight_sha256||!o->cpu_sha256||!o->catalog_sha256)return QW_ARGUMENT;
 qw_memory_plan p;int rc=qw_plan(o,weights->size_bytes,cpu->size_bytes,catalog->size_bytes,capacity,&p);if(rc)return rc;
 qw_model*m=(qw_model*)((unsigned char*)arena+p.context_offset);memset(m,0,sizeof *m);m->options=*o;m->plan=p;m->arena=arena;m->arena_capacity=capacity;
 memcpy(m->parameter_sha256,o->weight_sha256,32);
 memcpy(m->parameter_sha256+32,o->cpu_sha256,32);
 memcpy(m->parameter_sha256+64,o->catalog_sha256,32);
 if(npu_package_load_from_source(weights,0,weights->size_bytes,m->arena+p.weights_offset,p.weights_bytes,&m->package))return QW_FORMAT;
 if((rc=hash_matches(m->package.buffer,p.weights_bytes,o->weight_sha256))||
 (rc=load_source(cpu,m->arena+p.cpu_offset))||(rc=hash_matches(m->arena+p.cpu_offset,p.cpu_bytes,o->cpu_sha256))||
 (rc=load_source(catalog,m->arena+p.catalog_offset))||(rc=hash_matches(m->arena+p.catalog_offset,p.catalog_bytes,o->catalog_sha256)))return rc;
 if((rc=qw_bind_catalog(m,(const char*)m->arena+p.catalog_offset,p.catalog_bytes)))return rc;
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
