#include "qw_internal.h"
#include "qw_tensor_async.h"
#include "qw_checkpoint.h"
#include "qw_sha256.h"
#include <limits.h>
#include <string.h>
#ifdef QW_ENABLE_TENSOR_V2

#define CK_HEADER_BYTES 192u
#define CK_HASH_OFFSET 160u
#define CK_HASH_BYTES 32u
#define CK_IDENTITY_HASH_OFFSET 124u
#define CK_VERSION 1u
static const unsigned char ck_magic[8]={'Q','W','C','K','P','T','1',0};

typedef struct {uint32_t state_bytes,total_bytes;} ck_size;

static int overlap(const void *a,size_t an,const void *b,size_t bn)
{
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 if(!an||!bn)return 0;
 if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return 1;
 return x<y+bn&&y<x+an;
}
static void put32(unsigned char*p,uint32_t x)
{p[0]=(unsigned char)x;p[1]=(unsigned char)(x>>8);p[2]=(unsigned char)(x>>16);p[3]=(unsigned char)(x>>24);}
static uint32_t get32(const unsigned char*p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put64(unsigned char*p,uint64_t x)
{put32(p,(uint32_t)x);put32(p+4,(uint32_t)(x>>32));}
static uint64_t get64(const unsigned char*p)
{return get32(p)|((uint64_t)get32(p+4)<<32);}
static uint32_t features(const qw_model*m)
{
 uint32_t f=0;
#ifdef QW_ENABLE_TENSOR_V2
 f|=m->tensor_status.enabled?1u:0u;
#endif
 f|=m->options.lut_nonlinear?2u:0u;
 f|=m->head_candidate_allowed?4u:0u;
 f|=m->output_head?8u:0u;
 f|=m->head_integer_enabled?16u:0u;
 f|=m->output_head?((m->output_head->dtype&15u)<<8):0u;
 return f;
}
static int model_check(const qw_model*m,ck_size*out)
{
 if(!m||m->magic!=QW_MAGIC||!out)return QW_ARGUMENT;
#ifndef QW_ENABLE_TENSOR_V2
 return QW_FORMAT;
#else
 if(!m->tensor_status.enabled||!m->tensor_execute||!m->tensor_scratch||
    m->tensor_status.numeric_version!=NPU_TENSOR_NUMERIC_VERSION)return QW_FORMAT;
 if(m->raw.active||m->raw.cache_unknown||m->tensor_status.unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
 if(m->needs_reset)return QW_NEED_RESET;
 if(!m->arena||!m->options.weight_sha256||!m->options.cpu_sha256||!m->options.catalog_sha256||m->plan.state_bytes==0||m->plan.state_bytes>UINT32_MAX||
    m->plan.arena_bytes>UINT32_MAX||m->arena_capacity<m->plan.arena_bytes||
    m->plan.state_offset>m->plan.arena_bytes||
    m->plan.state_bytes>m->plan.arena_bytes-m->plan.state_offset||
    m->options.context_tokens==0||m->position>m->options.context_tokens||
    m->tensor_scratch_bytes>UINT32_MAX||m->plan.arena_bytes>UINT32_MAX-CK_HEADER_BYTES)
  return QW_FORMAT;
 uint64_t total=(uint64_t)CK_HEADER_BYTES+m->plan.state_bytes;
 if(total>UINT32_MAX||total>SIZE_MAX)return QW_CAPACITY;
 out->state_bytes=(uint32_t)m->plan.state_bytes;out->total_bytes=(uint32_t)total;
 return 0;
#endif
}
#ifdef QW_ENABLE_TENSOR_V2
static uint32_t model_mode(const qw_model*m)
{return (uint32_t)m->raw.config.mode;}
static void identity_write(const qw_model*m,unsigned char*h)
{
 put32(h+24,m->position);
 put32(h+28,m->options.context_tokens);
 put32(h+32,m->tensor_status.numeric_version);
 put32(h+36,model_mode(m));
 put32(h+40,features(m));
 put32(h+44,(uint32_t)m->plan.arena_bytes);
 put32(h+48,(uint32_t)m->arena_capacity);
 put32(h+52,(uint32_t)m->plan.state_offset);
 put64(h+56,(uint64_t)(uintptr_t)m);
 put64(h+64,(uint64_t)(uintptr_t)m->arena);
 put64(h+72,(uint64_t)(uintptr_t)m->package.buffer);
 put64(h+80,(uint64_t)(uintptr_t)m->output_head);
 put64(h+88,(uint64_t)(uintptr_t)m->tensor_scratch);
 put32(h+96,m->tensor_ddr);
 put32(h+100,(uint32_t)m->tensor_scratch_bytes);
 put32(h+104,m->tensor_count);
 put32(h+108,m->plan.policy_version);
 put32(h+112,(uint32_t)m->plan.weights_bytes);
 put32(h+116,(uint32_t)m->plan.cpu_bytes);
 put32(h+120,(uint32_t)m->plan.catalog_bytes);
 qw_sha256 hs;unsigned char identity[32];qw_sha_init(&hs);
 qw_sha_update(&hs,m->options.weight_sha256,32);qw_sha_update(&hs,m->options.cpu_sha256,32);
 qw_sha_update(&hs,m->options.catalog_sha256,32);qw_sha_final(&hs,identity);
 memcpy(h+CK_IDENTITY_HASH_OFFSET,identity,sizeof identity);
}
static int buffer_check(const qw_model*m,const void*buffer,size_t bytes,size_t need)
{
 if(!buffer||((uintptr_t)buffer&(QW_CHECKPOINT_ALIGNMENT-1u)))return QW_ARGUMENT;
 if(bytes<need)return QW_CAPACITY;
 if(overlap(buffer,need,m,sizeof *m)||
    overlap(buffer,need,m->arena,m->arena_capacity)||
    overlap(buffer,need,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(buffer,need,m->package.buffer,m->package.bytes))return QW_ARGUMENT;
 return 0;
}
static void digest(const unsigned char*image,uint32_t state_bytes,unsigned char out[32])
{
 qw_sha256 s;qw_sha_init(&s);qw_sha_update(&s,image,CK_HASH_OFFSET);
 qw_sha_update(&s,image+CK_HEADER_BYTES,state_bytes);qw_sha_final(&s,out);
}
int qw_checkpoint_size(const qw_model*m,size_t*bytes)
{
 ck_size s;int rc;if(!bytes)return QW_ARGUMENT;
 rc=model_check(m,&s);if(rc)return rc;
 if(overlap(bytes,sizeof *bytes,m,sizeof *m)||overlap(bytes,sizeof *bytes,m->arena,m->arena_capacity)||
    overlap(bytes,sizeof *bytes,m->tensor_scratch,m->tensor_scratch_bytes)||
    overlap(bytes,sizeof *bytes,m->package.buffer,m->package.bytes))return QW_ARGUMENT;
 *bytes=s.total_bytes;return 0;
}
int qw_checkpoint_save(const qw_model*m,void*buffer,size_t capacity)
{
 ck_size s;int rc=model_check(m,&s);if(rc)return rc;
 if((rc=buffer_check(m,buffer,capacity,s.total_bytes)))return rc;
 unsigned char*h=buffer;memset(h,0,CK_HEADER_BYTES);
 memcpy(h,ck_magic,sizeof ck_magic);put32(h+8,CK_VERSION);put32(h+12,CK_HEADER_BYTES);
 put32(h+16,s.total_bytes);put32(h+20,s.state_bytes);identity_write(m,h);
 memcpy(h+CK_HEADER_BYTES,m->arena+m->plan.state_offset,s.state_bytes);
 digest(h,s.state_bytes,h+CK_HASH_OFFSET);
 return 0;
}
static int header_check(const unsigned char*h,size_t bytes,uint32_t*state_bytes)
{
 if(bytes<CK_HEADER_BYTES||memcmp(h,ck_magic,sizeof ck_magic)||get32(h+8)!=CK_VERSION||
    get32(h+12)!=CK_HEADER_BYTES)return QW_FORMAT;
 uint32_t state=get32(h+20),total=get32(h+16);
 if(state>UINT32_MAX-CK_HEADER_BYTES||total!=CK_HEADER_BYTES+state||bytes<total||!state)
  return QW_FORMAT;
 if(h[156]||h[157]||h[158]||h[159])return QW_FORMAT;
 unsigned char actual[32];digest(h,state,actual);
 if(memcmp(actual,h+CK_HASH_OFFSET,sizeof actual))return QW_HASH;
 *state_bytes=state;return 0;
}
static int identity_check(const qw_model*m,const unsigned char*h,const ck_size*s)
{
 qw_sha256 hs;unsigned char identity[32];qw_sha_init(&hs);
 qw_sha_update(&hs,m->options.weight_sha256,32);qw_sha_update(&hs,m->options.cpu_sha256,32);
 qw_sha_update(&hs,m->options.catalog_sha256,32);qw_sha_final(&hs,identity);
 if(memcmp(h+CK_IDENTITY_HASH_OFFSET,identity,sizeof identity)||
    get32(h+20)!=s->state_bytes||get32(h+24)>m->options.context_tokens||
    get32(h+28)!=m->options.context_tokens||
    get32(h+32)!=m->tensor_status.numeric_version||get32(h+36)!=model_mode(m)||
    get32(h+40)!=features(m)||get32(h+44)!=(uint32_t)m->plan.arena_bytes||
    get32(h+48)!=(uint32_t)m->arena_capacity||get32(h+52)!=(uint32_t)m->plan.state_offset||
    get64(h+56)!=(uint64_t)(uintptr_t)m||get64(h+64)!=(uint64_t)(uintptr_t)m->arena||
    get64(h+72)!=(uint64_t)(uintptr_t)m->package.buffer||
    get64(h+80)!=(uint64_t)(uintptr_t)m->output_head||
    get64(h+88)!=(uint64_t)(uintptr_t)m->tensor_scratch||get32(h+96)!=m->tensor_ddr||
    get32(h+100)!=(uint32_t)m->tensor_scratch_bytes||get32(h+104)!=m->tensor_count||
    get32(h+108)!=m->plan.policy_version||get32(h+112)!=(uint32_t)m->plan.weights_bytes||
    get32(h+116)!=(uint32_t)m->plan.cpu_bytes||get32(h+120)!=(uint32_t)m->plan.catalog_bytes)
  return QW_FORMAT;
 return 0;
}
#else
static void identity_write(const qw_model*m,unsigned char*h){(void)m;(void)h;}
static int identity_check(const qw_model*m,const unsigned char*h,const ck_size*s)
{(void)m;(void)h;(void)s;return QW_FORMAT;}
static int buffer_check(const qw_model*m,const void*b,size_t n,size_t need)
{(void)m;(void)b;(void)n;(void)need;return QW_FORMAT;}
static void digest(const unsigned char*i,uint32_t n,unsigned char o[32])
{(void)i;(void)n;memset(o,0,32);}
#endif
int qw_checkpoint_restore(qw_model*m,const void*buffer,size_t bytes)
{
 ck_size s;uint32_t state_bytes;int rc=model_check(m,&s);if(rc)return rc;
 if(!buffer||((uintptr_t)buffer&(QW_CHECKPOINT_ALIGNMENT-1u)))return QW_ARGUMENT;
 if(bytes<CK_HEADER_BYTES)return QW_CAPACITY;
 if(overlap(buffer,CK_HEADER_BYTES,m,sizeof *m)||overlap(buffer,CK_HEADER_BYTES,m->arena,m->arena_capacity)||
    overlap(buffer,CK_HEADER_BYTES,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(buffer,CK_HEADER_BYTES,m->package.buffer,m->package.bytes))return QW_ARGUMENT;
 const unsigned char*h=buffer;
 if((rc=header_check(h,bytes,&state_bytes)))return rc;
 if(overlap(buffer,CK_HEADER_BYTES+(size_t)state_bytes,m,sizeof *m)||
    overlap(buffer,CK_HEADER_BYTES+(size_t)state_bytes,m->arena,m->arena_capacity)||
    overlap(buffer,CK_HEADER_BYTES+(size_t)state_bytes,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(buffer,CK_HEADER_BYTES+(size_t)state_bytes,m->package.buffer,m->package.bytes))return QW_ARGUMENT;
 if((rc=identity_check(m,h,&s)))return rc;
 memcpy(m->arena+m->plan.state_offset,h+CK_HEADER_BYTES,state_bytes);
 m->position=get32(h+24);
 m->logits_valid=0;
 return 0;
}
#else
int qw_checkpoint_size(const qw_model*m,size_t*bytes)
{(void)m;(void)bytes;return QW_FORMAT;}
int qw_checkpoint_save(const qw_model*m,void*buffer,size_t capacity)
{(void)m;(void)buffer;(void)capacity;return QW_FORMAT;}
int qw_checkpoint_restore(qw_model*m,const void*buffer,size_t bytes)
{(void)m;(void)buffer;(void)bytes;return QW_FORMAT;}
#endif
