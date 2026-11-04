#ifndef QW_PREFIX_CACHE_H
#define QW_PREFIX_CACHE_H
#include "qw_checkpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { QW_PREFIX_CACHE_EMPTY=0,QW_PREFIX_CACHE_BOUND=1,QW_PREFIX_CACHE_VALID=2,
 QW_PREFIX_MISS=-112 };
typedef struct {
 uint32_t magic,state,capacity_tokens,token_count;
 uint32_t *tokens;
 size_t token_bytes,checkpoint_capacity;
 void *checkpoint;
 qw_model *model;
} qw_prefix_cache;

/* Required bytes are separate from model DDR planning; caller reserves them. */
int qw_prefix_cache_required(const qw_model *model,uint32_t capacity_tokens,
 size_t *token_bytes,size_t *checkpoint_bytes,size_t *total_bytes);
/* Cache object must be zero initialized. A bound object cannot be rebound. */
int qw_prefix_cache_bind(qw_prefix_cache *cache,qw_model *model,
 uint32_t *tokens,size_t token_bytes,void *checkpoint,size_t checkpoint_bytes);
/* Save only a caller-supplied prefix already fully present in model state. */
int qw_prefix_cache_prime(qw_prefix_cache *cache,const uint32_t *executed_tokens,
 size_t token_count);
/* Full-prefix match restores state; miss never changes model or prompt. */
int qw_prefix_cache_reuse(qw_prefix_cache *cache,const uint32_t *prompt_tokens,
 size_t prompt_count,size_t *matched_prefix_count);
/* Invalidate only while all model/device leases are known idle. */
int qw_prefix_cache_invalidate(qw_prefix_cache *cache);

#ifdef __cplusplus
}
#endif
#endif
