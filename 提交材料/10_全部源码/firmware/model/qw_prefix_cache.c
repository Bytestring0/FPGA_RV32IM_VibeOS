#include "qw_internal.h"
#include "qw_tensor_async.h"
#include "qw_prefix_cache.h"
#include <limits.h>
#include <string.h>
#ifdef QW_ENABLE_TENSOR_V2

#define PREFIX_MAGIC UINT32_C(0x51575043)
static int overlap(const void*a,size_t an,const void*b,size_t bn)
{
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;if(!an||!bn)return 0;
 if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return 1;return x<y+bn&&y<x+an;
}
static int idle(qw_model*m)
{
 if(!m||m->magic!=QW_MAGIC)return QW_ARGUMENT;
#ifdef QW_ENABLE_TENSOR_V2
 if(m->tensor_status.unknown||m->raw.active||m->raw.cache_unknown)return QW_UNKNOWN;
 if(m->task_active||m->inside_step)return QW_BUSY;
 if(!m->tensor_status.enabled)return QW_FORMAT;
#else
 return QW_FORMAT;
#endif
 if(m->needs_reset)return QW_NEED_RESET;return 0;
}
static int cache_valid(const qw_prefix_cache*c)
{
 if(!c||c->magic!=PREFIX_MAGIC||!c->model||!c->tokens||!c->checkpoint||
    !c->capacity_tokens||c->token_count>c->capacity_tokens||
    c->token_bytes<(size_t)c->capacity_tokens*sizeof(uint32_t)||
    c->state<QW_PREFIX_CACHE_BOUND||c->state>QW_PREFIX_CACHE_VALID)return QW_ARGUMENT;
 return 0;
}
int qw_prefix_cache_required(const qw_model*m,uint32_t capacity_tokens,
 size_t*token_bytes,size_t*checkpoint_bytes,size_t*total_bytes)
{
 size_t ck,tok,total;
 if(!token_bytes||!checkpoint_bytes||!total_bytes||!capacity_tokens)return QW_ARGUMENT;
 size_t token_count=(size_t)capacity_tokens;
 if(token_count>SIZE_MAX/sizeof(uint32_t))return QW_CAPACITY;
 int rc=qw_checkpoint_size(m,&ck);if(rc)return rc;
 if(capacity_tokens>m->options.context_tokens)return QW_CAPACITY;
 if(overlap(token_bytes,sizeof *token_bytes,checkpoint_bytes,sizeof *checkpoint_bytes)||
    overlap(token_bytes,sizeof *token_bytes,total_bytes,sizeof *total_bytes)||
    overlap(checkpoint_bytes,sizeof *checkpoint_bytes,total_bytes,sizeof *total_bytes)||
    overlap(token_bytes,sizeof *token_bytes,m,sizeof *m)||overlap(checkpoint_bytes,sizeof *checkpoint_bytes,m,sizeof *m)||overlap(total_bytes,sizeof *total_bytes,m,sizeof *m)||
    overlap(token_bytes,sizeof *token_bytes,m->arena,m->arena_capacity)||overlap(checkpoint_bytes,sizeof *checkpoint_bytes,m->arena,m->arena_capacity)||overlap(total_bytes,sizeof *total_bytes,m->arena,m->arena_capacity)||
    overlap(token_bytes,sizeof *token_bytes,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(checkpoint_bytes,sizeof *checkpoint_bytes,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(total_bytes,sizeof *total_bytes,m->tensor_scratch,m->tensor_scratch_bytes)||
    overlap(token_bytes,sizeof *token_bytes,m->package.buffer,m->package.bytes)||overlap(checkpoint_bytes,sizeof *checkpoint_bytes,m->package.buffer,m->package.bytes)||overlap(total_bytes,sizeof *total_bytes,m->package.buffer,m->package.bytes))return QW_ARGUMENT;
 tok=(size_t)capacity_tokens*sizeof(uint32_t);
 if(tok>SIZE_MAX-ck)return QW_CAPACITY;total=tok+ck;
 *token_bytes=tok;*checkpoint_bytes=ck;*total_bytes=total;return 0;
}
int qw_prefix_cache_bind(qw_prefix_cache*c,qw_model*m,uint32_t*tokens,size_t token_bytes,
 void*checkpoint,size_t checkpoint_bytes)
{
 if(!c||!m||m->magic!=QW_MAGIC||!tokens||!checkpoint||!token_bytes||!checkpoint_bytes)return QW_ARGUMENT;
 if(c->magic!=0){if(c->magic==PREFIX_MAGIC&&c->model){int prior=idle(c->model);if(prior==QW_UNKNOWN)return prior;}return QW_ARGUMENT;}
 int rc=idle(m);if(rc)return rc;size_t need_ck;int size_rc=qw_checkpoint_size(m,&need_ck);if(size_rc)return size_rc;
 if(((uintptr_t)tokens&3u)||((uintptr_t)checkpoint&(QW_CHECKPOINT_ALIGNMENT-1u)))return QW_ARGUMENT;
 if(token_bytes%sizeof(uint32_t)||token_bytes/sizeof(uint32_t)>UINT32_MAX)return QW_ARGUMENT;
 if(checkpoint_bytes<need_ck)return QW_CAPACITY;
 size_t count=token_bytes/sizeof(uint32_t);
 if(overlap(c,sizeof *c,m,sizeof *m)||overlap(c,sizeof *c,m->arena,m->arena_capacity)||
    overlap(c,sizeof *c,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(c,sizeof *c,m->package.buffer,m->package.bytes)||
    overlap(tokens,token_bytes,checkpoint,checkpoint_bytes)||
    overlap(c,sizeof *c,tokens,token_bytes)||overlap(c,sizeof *c,checkpoint,checkpoint_bytes)||
    overlap(tokens,token_bytes,m,sizeof *m)||overlap(tokens,token_bytes,m->arena,m->arena_capacity)||
    overlap(tokens,token_bytes,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(tokens,token_bytes,m->package.buffer,m->package.bytes)||
    overlap(checkpoint,checkpoint_bytes,m,sizeof *m)||overlap(checkpoint,checkpoint_bytes,m->arena,m->arena_capacity)||
    overlap(checkpoint,checkpoint_bytes,m->tensor_scratch,m->tensor_scratch_bytes)||overlap(checkpoint,checkpoint_bytes,m->package.buffer,m->package.bytes))return QW_ARGUMENT;
 if(count==0||count>UINT32_MAX)return QW_ARGUMENT;
 c->model=m;c->tokens=tokens;c->token_bytes=token_bytes;c->capacity_tokens=(uint32_t)count;
 c->checkpoint=checkpoint;c->checkpoint_capacity=checkpoint_bytes;c->token_count=0;c->state=QW_PREFIX_CACHE_BOUND;c->magic=PREFIX_MAGIC;
 return 0;
}
int qw_prefix_cache_prime(qw_prefix_cache*c,const uint32_t*executed,size_t count)
{
 int rc=cache_valid(c);if(rc)return rc;rc=idle(c->model);if(rc)return rc;
 if((!executed&&count)||count>c->capacity_tokens||count>UINT32_MAX)return count>c->capacity_tokens?QW_CAPACITY:QW_ARGUMENT;
 if((executed!=c->tokens&&overlap(executed,count*sizeof(uint32_t),c->tokens,c->token_bytes))||
    overlap(executed,count*sizeof(uint32_t),c->checkpoint,c->checkpoint_capacity)||
    overlap(executed,count*sizeof(uint32_t),c,sizeof *c)||
    overlap(executed,count*sizeof(uint32_t),c->model,sizeof *c->model)||
    overlap(executed,count*sizeof(uint32_t),c->model->arena,c->model->arena_capacity)||
    overlap(executed,count*sizeof(uint32_t),c->model->tensor_scratch,c->model->tensor_scratch_bytes))return QW_ARGUMENT;
 if(c->model->position!=count)return QW_NEED_RESET;
 c->state=QW_PREFIX_CACHE_BOUND;c->token_count=0;
 rc=qw_checkpoint_save(c->model,c->checkpoint,c->checkpoint_capacity);if(rc)return rc;
 if(count&&executed!=c->tokens)memcpy(c->tokens,executed,count*sizeof(uint32_t));
 c->token_count=(uint32_t)count;c->state=QW_PREFIX_CACHE_VALID;return 0;
}
int qw_prefix_cache_reuse(qw_prefix_cache*c,const uint32_t*prompt,size_t prompt_count,size_t*matched)
{
 int rc=cache_valid(c);if(rc)return rc;rc=idle(c->model);if(rc)return rc;
 if(!matched||(!prompt&&prompt_count)||prompt_count>SIZE_MAX/sizeof(uint32_t))return QW_ARGUMENT;
 size_t prompt_bytes=prompt_count*sizeof(uint32_t);
 if(overlap(matched,sizeof *matched,c,sizeof *c)||
    overlap(matched,sizeof *matched,c->model,sizeof *c->model)||
    overlap(matched,sizeof *matched,c->model->arena,c->model->arena_capacity)||
    overlap(matched,sizeof *matched,c->model->tensor_scratch,c->model->tensor_scratch_bytes)||
    overlap(matched,sizeof *matched,c->model->package.buffer,c->model->package.bytes)||
    overlap(matched,sizeof *matched,c->tokens,c->token_bytes)||
    overlap(matched,sizeof *matched,c->checkpoint,c->checkpoint_capacity)||
    overlap(matched,sizeof *matched,prompt,prompt_bytes))return QW_ARGUMENT;
 if(overlap(prompt,prompt_bytes,c,sizeof *c)||
    overlap(prompt,prompt_bytes,c->model,sizeof *c->model)||
    overlap(prompt,prompt_bytes,c->model->arena,c->model->arena_capacity)||
    overlap(prompt,prompt_bytes,c->model->tensor_scratch,c->model->tensor_scratch_bytes)||
    overlap(prompt,prompt_bytes,c->checkpoint,c->checkpoint_capacity))return QW_ARGUMENT;
 *matched=0;
 if(c->state!=QW_PREFIX_CACHE_VALID||prompt_count<c->token_count)return QW_PREFIX_MISS;
 for(uint32_t i=0;i<c->token_count;i++)if(prompt[i]!=c->tokens[i])return QW_PREFIX_MISS;
 rc=qw_checkpoint_restore(c->model,c->checkpoint,c->checkpoint_capacity);if(rc)return rc;
 *matched=c->token_count;return 0;
}
int qw_prefix_cache_invalidate(qw_prefix_cache*c)
{
 int rc=cache_valid(c);if(rc)return rc;rc=idle(c->model);if(rc)return rc;
 c->state=QW_PREFIX_CACHE_BOUND;c->token_count=0;return 0;
}
#else
int qw_prefix_cache_required(const qw_model*m,uint32_t capacity_tokens,size_t*t,size_t*c,size_t*total)
{(void)m;(void)capacity_tokens;(void)t;(void)c;(void)total;return QW_FORMAT;}
int qw_prefix_cache_bind(qw_prefix_cache*c,qw_model*m,uint32_t*t,size_t tb,void*ck,size_t cb)
{(void)c;(void)m;(void)t;(void)tb;(void)ck;(void)cb;return QW_FORMAT;}
int qw_prefix_cache_prime(qw_prefix_cache*c,const uint32_t*t,size_t n)
{(void)c;(void)t;(void)n;return QW_FORMAT;}
int qw_prefix_cache_reuse(qw_prefix_cache*c,const uint32_t*t,size_t n,size_t*m)
{(void)c;(void)t;(void)n;(void)m;return QW_FORMAT;}
int qw_prefix_cache_invalidate(qw_prefix_cache*c)
{(void)c;return QW_FORMAT;}
#endif
