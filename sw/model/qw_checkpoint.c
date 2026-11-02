#include "qw_internal.h"
#include "qw_sha256.h"
#include <stddef.h>
#include <string.h>

/* This format is deliberately restricted to the existing FP32 software
 * reference. A future integer state/scales format needs its own version.
 * Offsets, not process pointers, are serialized. RV32 and host little-endian
 * builds use the same fixed 256-byte header. */
#define QW_CK_MAGIC UINT32_C(0x51434b31)
#define QW_CK_FORMAT_FP32 1u
typedef struct {
 uint32_t magic,format,policy_version,position,context_tokens,prefix_count;
 uint64_t state_bytes;
 uint8_t parameter_sha256[96],key_sha256[96],digest[32];
} qw_ck_header;
_Static_assert(sizeof(qw_ck_header)==256,"checkpoint header must be 256 bytes");

static int overlaps(const void *a,size_t an,const void *b,size_t bn){
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return 1;
 return x<y+bn&&y<x+an;
}
static int state_ok(const qw_model *m){
 return m&&m->magic==QW_MAGIC&&m->arena&&
  m->options.context_tokens&&m->options.context_tokens<=QW_CONTEXT_MAX&&
  m->plan.state_offset<=m->arena_capacity&&
  m->plan.state_bytes<=m->arena_capacity-m->plan.state_offset;
}
static void digest_image(const qw_ck_header *h,const unsigned char *payload,
                         size_t bytes,unsigned char result[32]){
 qw_sha256 s;qw_sha_init(&s);
 qw_sha_update(&s,h,offsetof(qw_ck_header,digest));
 qw_sha_update(&s,payload,bytes);qw_sha_final(&s,result);
}
static int key_ok(const qw_checkpoint_key *key){
 return key&&key->prefix_ids&&key->prefix_count&&
        key->prefix_count<=QW_CONTEXT_MAX;
}
static void bind_key(qw_ck_header *h,const qw_checkpoint_key *key){
 memcpy(h->key_sha256,key->tokenizer_sha256,32);
 memcpy(h->key_sha256+32,key->template_sha256,32);
 memcpy(h->key_sha256+64,key->schema_sha256,32);
}
int qw_checkpoint_bytes(const qw_model *m,size_t count,size_t *bytes){
 if(!state_ok(m)||!bytes||!count||count>m->options.context_tokens)return QW_ARGUMENT;
 if(count>(SIZE_MAX-sizeof(qw_ck_header))/sizeof(uint32_t))return QW_CAPACITY;
 size_t n=sizeof(qw_ck_header)+count*sizeof(uint32_t);
 if(m->plan.state_bytes>SIZE_MAX-n)return QW_CAPACITY;
 *bytes=n+m->plan.state_bytes;return QW_OK;
}
int qw_checkpoint_save(const qw_model *m,const qw_checkpoint_key *key,
                       void *buffer,size_t capacity){
 size_t bytes;
 if(!state_ok(m)||!key_ok(key)||!buffer)return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;
 if(m->execution.active||m->checkpoint_active)return QW_BUSY;
 if(m->needs_reset)return QW_NEED_RESET;
 int rc=qw_checkpoint_bytes(m,key->prefix_count,&bytes);if(rc)return rc;
 if(capacity<bytes)return QW_CAPACITY;
 if(m->position!=key->prefix_count||
    memcmp(m->processed_ids,key->prefix_ids,key->prefix_count*sizeof(uint32_t)))return QW_TOKEN;
 if(overlaps(buffer,bytes,m->arena,m->arena_capacity)||
    overlaps(buffer,bytes,key->prefix_ids,key->prefix_count*sizeof(uint32_t)))return QW_ARGUMENT;
 qw_ck_header h={0};h.magic=QW_CK_MAGIC;h.format=QW_CK_FORMAT_FP32;
 h.policy_version=QW_POLICY_VERSION;h.position=m->position;
 h.context_tokens=m->options.context_tokens;h.prefix_count=(uint32_t)key->prefix_count;
 h.state_bytes=m->plan.state_bytes;
 memcpy(h.parameter_sha256,m->parameter_sha256,sizeof h.parameter_sha256);bind_key(&h,key);
 unsigned char *payload=(unsigned char *)buffer+sizeof h;
 size_t id_bytes=key->prefix_count*sizeof(uint32_t);
 memcpy(payload,key->prefix_ids,id_bytes);
 memcpy(payload+id_bytes,m->arena+m->plan.state_offset,m->plan.state_bytes);
 digest_image(&h,payload,bytes-sizeof h,h.digest);
 memcpy(buffer,&h,sizeof h);return QW_OK;
}
int qw_checkpoint_restore(qw_model *m,const qw_checkpoint_key *key,
                          const void *buffer,size_t bytes){
 if(!state_ok(m)||!key_ok(key)||!buffer)return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;
 if(m->execution.active||m->checkpoint_active)return QW_BUSY;
 if(bytes<sizeof(qw_ck_header)||overlaps(buffer,bytes,m->arena,m->arena_capacity))return QW_ARGUMENT;
 qw_ck_header h;memcpy(&h,buffer,sizeof h);
 if(h.magic!=QW_CK_MAGIC||h.format!=QW_CK_FORMAT_FP32||
    h.policy_version!=QW_POLICY_VERSION||h.context_tokens!=m->options.context_tokens||
    !h.prefix_count||h.position!=h.prefix_count||h.prefix_count!=key->prefix_count||
    h.state_bytes!=m->plan.state_bytes)return QW_FORMAT;
 size_t required;int rc=qw_checkpoint_bytes(m,key->prefix_count,&required);
 if(rc||bytes!=required)return QW_FORMAT;
 if(memcmp(h.parameter_sha256,m->parameter_sha256,sizeof h.parameter_sha256))return QW_HASH;
 qw_ck_header wanted={0};bind_key(&wanted,key);
 if(memcmp(h.key_sha256,wanted.key_sha256,sizeof h.key_sha256))return QW_HASH;
 const unsigned char *payload=(const unsigned char *)buffer+sizeof h;
 size_t id_bytes=key->prefix_count*sizeof(uint32_t);
 if(memcmp(payload,key->prefix_ids,id_bytes))return QW_TOKEN;
 uint8_t digest[32];digest_image(&h,payload,bytes-sizeof h,digest);
 if(memcmp(digest,h.digest,sizeof digest))return QW_HASH;
 /* No calls that can fail after this point; publish position last. */
 memcpy(m->arena+m->plan.state_offset,payload+id_bytes,m->plan.state_bytes);
 memcpy(m->processed_ids,payload,id_bytes);
 memset(m->processed_ids+h.position,0,(QW_CONTEXT_MAX-h.position)*sizeof(uint32_t));
 if(m->w)memset(m->w,0,sizeof *m->w);
 memset(&m->execution,0,sizeof m->execution);
 m->position=h.position;m->logits_valid=0;m->needs_reset=0;m->last_error=0;
 return QW_OK;
}
int qw_checkpoint_restore_from_source(qw_model *m,const qw_checkpoint_key *key,
 const npu_storage_source *source,const uint8_t expected_sha256[32],
 void *buffer,size_t capacity){
 /* Lease gates precede any source/buffer access, including argument failures. */
 if(!state_ok(m))return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;
 if(m->execution.active||m->checkpoint_active)return QW_BUSY;
 if(!key_ok(key)||!source||!source->read_exact_at||!expected_sha256||!buffer)
  return QW_ARGUMENT;
 size_t bytes;int rc=qw_checkpoint_bytes(m,key->prefix_count,&bytes);
 if(rc)return rc;
 if(capacity<bytes)return QW_CAPACITY;
 /* Check the whole declared allocation, not just the bytes being loaded. */
 if(capacity>UINTPTR_MAX-(uintptr_t)buffer||
    overlaps(buffer,capacity,m,sizeof *m)||
    overlaps(buffer,capacity,m->arena,m->arena_capacity)||
    (m->w&&overlaps(buffer,capacity,m->w,sizeof *m->w))||
    overlaps(buffer,capacity,key,sizeof *key)||
    overlaps(buffer,capacity,key->prefix_ids,key->prefix_count*sizeof(uint32_t))||
    overlaps(buffer,capacity,source,sizeof *source)||
    (source->ctx&&overlaps(buffer,capacity,source->ctx,1))||
    overlaps(buffer,capacity,expected_sha256,32))return QW_ARGUMENT;
 if(source->size_bytes!=(uint64_t)bytes)return QW_FORMAT;
 /* The header stays local while I/O runs. A callback never sees a valid magic
  * in a partly overwritten external image, even if a later read fails. */
 qw_ck_header staged_header;
 memset(buffer,0,sizeof(uint32_t));
 if(source->read_exact_at(source->ctx,0,&staged_header,sizeof staged_header))
  return QW_IO;
 unsigned char *out=(unsigned char *)buffer;
 for(size_t offset=sizeof staged_header;offset<bytes;){
  size_t chunk=bytes-offset;if(chunk>4096u)chunk=4096u;
  if(source->read_exact_at(source->ctx,(uint64_t)offset,out+offset,chunk)){
   memset(buffer,0,sizeof(uint32_t));return QW_IO;
  }
  offset+=chunk;
 }
 uint8_t digest[32];qw_sha256 sha;qw_sha_init(&sha);
 qw_sha_update(&sha,&staged_header,sizeof staged_header);
 qw_sha_update(&sha,out+sizeof staged_header,bytes-sizeof staged_header);
 qw_sha_final(&sha,digest);
 if(memcmp(digest,expected_sha256,32))return QW_HASH;
 memcpy(buffer,&staged_header,sizeof staged_header);
 rc=qw_checkpoint_restore(m,key,buffer,bytes);
 if(rc)memset(buffer,0,sizeof(uint32_t));
 return rc;
}
int qw_checkpoint_invalidate(void *buffer,size_t bytes){
 if(!buffer||bytes<sizeof(qw_ck_header))return QW_ARGUMENT;
 memset(buffer,0,sizeof(uint32_t));return QW_OK;
}

#define QW_CK_JOB_MAGIC UINT32_C(0x434b4a31)
typedef struct {
 uint32_t magic;
 qw_checkpoint_progress progress;
 qw_checkpoint_job *self;
 qw_model *model;
 const qw_checkpoint_key *key;
 const uint32_t *ids;
 unsigned char *image,*arena;
 qw_work *work;
 size_t span,arena_capacity,state_offset,state_bytes,id_bytes,offset;
 uint32_t context_tokens,old_position,old_needs_reset,old_logits_valid;
 qw_ck_header header;
 qw_sha256 sha;
} qw_ck_job_state;
_Static_assert(sizeof(qw_ck_job_state)<=QW_CHECKPOINT_JOB_BYTES,"checkpoint job storage");

/* memcpy avoids aliasing an opaque public byte-array with an internal struct. */
static void job_store(const qw_ck_job_state *s){memcpy(s->self->opaque,s,sizeof *s);}
static int job_range_ok(const qw_checkpoint_job *job){
 uintptr_t address=(uintptr_t)job;
 return job&&address%_Alignof(qw_checkpoint_job)==0&&sizeof *job<=UINTPTR_MAX-address;
}
static int job_load(const qw_checkpoint_job *job,qw_ck_job_state *s){
 if(!job_range_ok(job))return QW_ARGUMENT;
 memcpy(s,job->opaque,sizeof *s);
 return s->magic==QW_CK_JOB_MAGIC&&s->self==job?QW_OK:QW_ARGUMENT;
}
static int job_aliases(const qw_model *m,const qw_checkpoint_key *key,
                       const qw_checkpoint_job *job,const void *image,size_t span){
 size_t ids=key->prefix_count*sizeof(uint32_t);
 if(span>UINTPTR_MAX-(uintptr_t)image)return 1;
 if(overlaps(job,sizeof *job,image,span)||overlaps(job,sizeof *job,m,sizeof *m)||
    overlaps(job,sizeof *job,m->arena,m->arena_capacity)||
    (m->w&&overlaps(job,sizeof *job,m->w,sizeof *m->w))||
    overlaps(job,sizeof *job,key,sizeof *key)||overlaps(job,sizeof *job,key->prefix_ids,ids))return 1;
 if(overlaps(image,span,m,sizeof *m)||overlaps(image,span,m->arena,m->arena_capacity)||
    (m->w&&overlaps(image,span,m->w,sizeof *m->w))||
    overlaps(image,span,key,sizeof *key)||overlaps(image,span,key->prefix_ids,ids))return 1;
 /* Borrowed key storage cannot be destroyed by restore state/work/ID writes. */
 if(overlaps(key,sizeof *key,m,sizeof *m)||overlaps(key,sizeof *key,m->arena,m->arena_capacity)||
    overlaps(key->prefix_ids,ids,m,sizeof *m)||overlaps(key->prefix_ids,ids,m->arena,m->arena_capacity)||
    (m->w&&(overlaps(key,sizeof *key,m->w,sizeof *m->w)||
            overlaps(key->prefix_ids,ids,m->w,sizeof *m->w))))return 1;
 return 0;
}
static int job_header_ok(const qw_model *m,const qw_checkpoint_key *key,
                         const qw_ck_header *h){
 if(h->magic!=QW_CK_MAGIC||h->format!=QW_CK_FORMAT_FP32||
    h->policy_version!=QW_POLICY_VERSION||h->context_tokens!=m->options.context_tokens||
    !h->prefix_count||h->prefix_count!=key->prefix_count||h->position!=h->prefix_count||
    h->state_bytes!=m->plan.state_bytes)return QW_FORMAT;
 if(memcmp(h->parameter_sha256,m->parameter_sha256,96))return QW_HASH;
 qw_ck_header wanted={0};bind_key(&wanted,key);
 return memcmp(h->key_sha256,wanted.key_sha256,96)?QW_HASH:QW_OK;
}
static int job_begin(qw_checkpoint_job *job,qw_model *m,const qw_checkpoint_key *key,
                      void *image,size_t span,int restoring){
 if(!state_ok(m))return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;
 if(m->execution.active||m->checkpoint_active)return QW_BUSY;
 if(!job_range_ok(job)||!key_ok(key)||!image)return QW_ARGUMENT;
 if(!restoring&&m->needs_reset)return QW_NEED_RESET;
 size_t bytes;int rc=qw_checkpoint_bytes(m,key->prefix_count,&bytes);if(rc)return rc;
 if(span<bytes)return restoring?QW_FORMAT:QW_CAPACITY;
 if(restoring&&span!=bytes)return QW_FORMAT;
 if(job_aliases(m,key,job,image,span))return QW_ARGUMENT;
 qw_ck_job_state old;memcpy(&old,job->opaque,sizeof old);
 if(old.magic==QW_CK_JOB_MAGIC&&old.progress.active)return QW_BUSY;
 if(old.magic&&old.magic!=QW_CK_JOB_MAGIC)return QW_ARGUMENT;
 if(!restoring&&m->position!=key->prefix_count)return QW_TOKEN;
 qw_ck_job_state s={0};s.magic=QW_CK_JOB_MAGIC;s.self=job;s.model=m;
 s.key=key;s.ids=key->prefix_ids;s.image=image;s.span=span;s.arena=m->arena;
 s.arena_capacity=m->arena_capacity;s.state_offset=m->plan.state_offset;
 s.state_bytes=m->plan.state_bytes;s.work=m->w;s.id_bytes=key->prefix_count*sizeof(uint32_t);
 s.context_tokens=m->options.context_tokens;s.old_position=m->position;
 s.old_needs_reset=m->needs_reset;s.old_logits_valid=m->logits_valid;
 s.progress.active=1;s.progress.restoring=(uint32_t)restoring;
 s.progress.image_bytes=bytes;s.progress.result=QW_PENDING;
 if(restoring){
  memcpy(&s.header,image,sizeof s.header);rc=job_header_ok(m,key,&s.header);if(rc)return rc;
  s.progress.phase=QW_CHECKPOINT_VERIFY_IDS;
 }else{
  s.header.magic=QW_CK_MAGIC;s.header.format=QW_CK_FORMAT_FP32;
  s.header.policy_version=QW_POLICY_VERSION;s.header.position=m->position;
  s.header.context_tokens=m->options.context_tokens;s.header.prefix_count=(uint32_t)key->prefix_count;
  s.header.state_bytes=m->plan.state_bytes;memcpy(s.header.parameter_sha256,m->parameter_sha256,96);
  bind_key(&s.header,key);s.progress.phase=QW_CHECKPOINT_SAVE_IDS;
 }
 qw_sha_init(&s.sha);qw_sha_update(&s.sha,&s.header,offsetof(qw_ck_header,digest));
 if(!restoring)memset(image,0,sizeof(uint32_t));
 memset(job,0,sizeof *job);job_store(&s);m->checkpoint_job=job;m->checkpoint_active=1;
 return QW_OK;
}
int qw_checkpoint_begin_save(qw_checkpoint_job *job,qw_model *m,
 const qw_checkpoint_key *key,void *image,size_t capacity){
 return job_begin(job,m,key,image,capacity,0);
}
int qw_checkpoint_begin_restore(qw_checkpoint_job *job,qw_model *m,
 const qw_checkpoint_key *key,const void *image,size_t bytes){
 return job_begin(job,m,key,(void *)image,bytes,1);
}
int qw_checkpoint_begin_reset(qw_checkpoint_job *job,qw_model *m){
 if(!state_ok(m))return QW_ARGUMENT;
 if(m->raw.active)return QW_UNKNOWN;
 if(m->execution.active||m->checkpoint_active)return QW_BUSY;
 if(!job_range_ok(job)||overlaps(job,sizeof *job,m,sizeof *m)||
    overlaps(job,sizeof *job,m->arena,m->arena_capacity)||
    (m->w&&overlaps(job,sizeof *job,m->w,sizeof *m->w)))return QW_ARGUMENT;
 qw_ck_job_state old;memcpy(&old,job->opaque,sizeof old);
 if(old.magic==QW_CK_JOB_MAGIC&&old.progress.active)return QW_BUSY;
 if(old.magic&&old.magic!=QW_CK_JOB_MAGIC)return QW_ARGUMENT;
 qw_ck_job_state s={0};s.magic=QW_CK_JOB_MAGIC;s.self=job;s.model=m;
 s.arena=m->arena;s.arena_capacity=m->arena_capacity;s.state_offset=m->plan.state_offset;
 s.state_bytes=m->plan.state_bytes;s.work=m->w;s.context_tokens=m->options.context_tokens;
 s.old_position=m->position;s.old_needs_reset=m->needs_reset;s.old_logits_valid=m->logits_valid;
 memcpy(s.header.parameter_sha256,m->parameter_sha256,96);
 s.progress.active=1;s.progress.resetting=1;s.progress.phase=QW_CHECKPOINT_RESET_STATE;
 s.progress.result=QW_PENDING;
 memset(job,0,sizeof *job);job_store(&s);m->checkpoint_job=job;m->checkpoint_active=1;
 return QW_OK;
}
static int job_identity(const qw_ck_job_state *s){
 const qw_model *m=s->model;
 if(!state_ok(m)||m->checkpoint_job!=s->self||!m->checkpoint_active||
    m->arena!=s->arena||m->arena_capacity!=s->arena_capacity||
    m->plan.state_offset!=s->state_offset||m->plan.state_bytes!=s->state_bytes||
    m->w!=s->work||m->options.context_tokens!=s->context_tokens||
    m->position!=s->old_position)return QW_ARGUMENT;
 if(m->needs_reset!=(s->progress.state_modified?1u:s->old_needs_reset)||
    m->logits_valid!=(s->progress.state_modified?0u:s->old_logits_valid))return QW_ARGUMENT;
 if(memcmp(m->parameter_sha256,s->header.parameter_sha256,96))return QW_HASH;
 if(s->progress.resetting)return QW_OK;
 if(s->key->prefix_ids!=s->ids||s->key->prefix_count!=s->header.prefix_count)return QW_TOKEN;
 qw_ck_header wanted={0};bind_key(&wanted,s->key);
 if(memcmp(wanted.key_sha256,s->header.key_sha256,96))return QW_HASH;
 if(s->progress.restoring){
  if(memcmp(s->image,&s->header,sizeof s->header))return QW_FORMAT;
 }else{
  uint32_t magic;memcpy(&magic,s->image,sizeof magic);if(magic)return QW_FORMAT;
 }
 return QW_OK;
}
static int job_finish(qw_ck_job_state *s,int reason){
 qw_model *m=s->model;int result=reason;
 if(reason!=QW_OK&&(s->progress.restoring||s->progress.resetting)&&s->progress.state_modified){
  m->needs_reset=1;m->logits_valid=0;m->last_error=QW_NEED_RESET;result=QW_NEED_RESET;
 }
 if(reason!=QW_OK&&!s->progress.restoring&&!s->progress.resetting)memset(s->image,0,sizeof(uint32_t));
 if(m->checkpoint_job==s->self){m->checkpoint_job=NULL;m->checkpoint_active=0;}
 s->progress.active=0;s->progress.phase=QW_CHECKPOINT_DONE;
 s->progress.result=result;s->progress.failure_reason=reason;job_store(s);return result;
}
static void job_mark_modified(qw_ck_job_state *s){
 if(!s->progress.state_modified){
  s->progress.state_modified=1;s->model->needs_reset=1;s->model->logits_valid=0;
 }
}
static size_t job_chunk(size_t remaining){
 return remaining>QW_CHECKPOINT_BLOCK_BYTES?QW_CHECKPOINT_BLOCK_BYTES:remaining;
}
static int job_unit(qw_ck_job_state *s){
 unsigned char *payload=s->image?s->image+sizeof(qw_ck_header):NULL;
 size_t n;uint8_t digest[32];
 switch(s->progress.phase){
 case QW_CHECKPOINT_RESET_STATE:
  n=job_chunk(s->state_bytes-s->offset);
  if(n){job_mark_modified(s);memset(s->arena+s->state_offset+s->offset,0,n);}
  s->offset+=n;s->progress.clear_bytes+=n;
  if(s->offset==s->state_bytes){s->offset=0;s->progress.phase=QW_CHECKPOINT_CLEAR_WORK;}
  return QW_PENDING;
 case QW_CHECKPOINT_SAVE_IDS:
  n=job_chunk(s->id_bytes-s->offset);
  if(memcmp((const unsigned char *)s->ids+s->offset,
            (const unsigned char *)s->model->processed_ids+s->offset,n))return QW_TOKEN;
  memcpy(payload+s->offset,(const unsigned char *)s->ids+s->offset,n);
  qw_sha_update(&s->sha,payload+s->offset,n);
  s->offset+=n;s->progress.hash_bytes+=n;s->progress.copy_bytes+=n;
  if(s->offset==s->id_bytes){s->offset=0;s->progress.phase=QW_CHECKPOINT_SAVE_STATE;}
  return QW_PENDING;
 case QW_CHECKPOINT_SAVE_STATE:
  n=job_chunk(s->state_bytes-s->offset);
  memcpy(payload+s->id_bytes+s->offset,s->arena+s->state_offset+s->offset,n);
  qw_sha_update(&s->sha,payload+s->id_bytes+s->offset,n);
  s->offset+=n;s->progress.hash_bytes+=n;s->progress.copy_bytes+=n;
  if(s->offset==s->state_bytes){
   qw_sha_final(&s->sha,s->header.digest);s->offset=0;s->progress.phase=QW_CHECKPOINT_COMMIT;
  }
  return QW_PENDING;
 case QW_CHECKPOINT_VERIFY_IDS:
  n=job_chunk(s->id_bytes-s->offset);
  if(memcmp(payload+s->offset,(const unsigned char *)s->ids+s->offset,n))return QW_TOKEN;
  qw_sha_update(&s->sha,payload+s->offset,n);s->offset+=n;s->progress.hash_bytes+=n;
  if(s->offset==s->id_bytes){s->offset=0;s->progress.phase=QW_CHECKPOINT_VERIFY_STATE;}
  return QW_PENDING;
 case QW_CHECKPOINT_VERIFY_STATE:
  n=job_chunk(s->state_bytes-s->offset);
  qw_sha_update(&s->sha,payload+s->id_bytes+s->offset,n);
  s->offset+=n;s->progress.hash_bytes+=n;
  if(s->offset==s->state_bytes){
   qw_sha_final(&s->sha,digest);if(memcmp(digest,s->header.digest,32))return QW_HASH;
   s->offset=0;s->progress.phase=QW_CHECKPOINT_RESTORE_STATE;
  }
  return QW_PENDING;
 case QW_CHECKPOINT_RESTORE_STATE:
  n=job_chunk(s->state_bytes-s->offset);
  if(n){job_mark_modified(s);memcpy(s->arena+s->state_offset+s->offset,payload+s->id_bytes+s->offset,n);}
  s->offset+=n;s->progress.copy_bytes+=n;
  if(s->offset==s->state_bytes){s->offset=0;s->progress.phase=QW_CHECKPOINT_CLEAR_WORK;}
  return QW_PENDING;
 case QW_CHECKPOINT_CLEAR_WORK:
  n=s->work?job_chunk(sizeof *s->work-s->offset):0;
  if(n){job_mark_modified(s);memset((unsigned char *)s->work+s->offset,0,n);}
  s->offset+=n;s->progress.clear_bytes+=n;
  if(!s->work||s->offset==sizeof *s->work){s->offset=0;s->progress.phase=QW_CHECKPOINT_PUBLISH_IDS;}
  return QW_PENDING;
 case QW_CHECKPOINT_PUBLISH_IDS:{
  n=job_chunk(sizeof s->model->processed_ids-s->offset);size_t copied=0;
  job_mark_modified(s);
  if(s->offset<s->id_bytes){copied=s->id_bytes-s->offset;if(copied>n)copied=n;
   memcpy((unsigned char *)s->model->processed_ids+s->offset,payload+s->offset,copied);}
  if(copied<n)memset((unsigned char *)s->model->processed_ids+s->offset+copied,0,n-copied);
  s->offset+=n;s->progress.copy_bytes+=copied;s->progress.clear_bytes+=n-copied;
  if(s->offset==sizeof s->model->processed_ids){s->offset=0;s->progress.phase=QW_CHECKPOINT_COMMIT;}
  return QW_PENDING;
 }
 case QW_CHECKPOINT_COMMIT:
  /* Recheck borrowed exact IDs at the end, still at most one 4KB block. */
  if(s->offset<s->id_bytes){
   n=job_chunk(s->id_bytes-s->offset);
   if(memcmp(payload+s->offset,(const unsigned char *)s->ids+s->offset,n))return QW_TOKEN;
   if(!s->progress.restoring&&memcmp(payload+s->offset,
      (const unsigned char *)s->model->processed_ids+s->offset,n))return QW_TOKEN;
   s->offset+=n;return QW_PENDING;
  }
  if(s->progress.restoring||s->progress.resetting){
   memset(&s->model->execution,0,sizeof s->model->execution);
   s->model->logits_valid=0;s->model->needs_reset=0;s->model->last_error=0;
   s->model->position=s->header.position;
  }else memcpy(s->image,&s->header,sizeof s->header);
  return QW_OK;
 default:return QW_ARGUMENT;
 }
}
int qw_checkpoint_poll(qw_checkpoint_job *job,uint32_t work_quota,uint32_t cpu_budget_ms){
 qw_ck_job_state s;int rc=job_load(job,&s);if(rc)return rc;
 if(!s.progress.active)return s.progress.result;
 s.progress.last_poll_units=0;
 if(!state_ok(s.model))return job_finish(&s,QW_ARGUMENT);
 if(s.model->raw.active){s.progress.failure_reason=QW_UNKNOWN;job_store(&s);return QW_UNKNOWN;}
 if(s.model->execution.active){s.progress.failure_reason=QW_BUSY;job_store(&s);return QW_BUSY;}
 if(!work_quota||(cpu_budget_ms&&!s.model->options.now_ms)){job_store(&s);return QW_ARGUMENT;}
 uint64_t begin=cpu_budget_ms?s.model->options.now_ms(s.model->options.clock_user):0;
 s.progress.failure_reason=QW_OK;
 while(s.progress.last_poll_units<work_quota){
  rc=job_identity(&s);if(rc)return job_finish(&s,rc);
  ++s.progress.last_poll_units;rc=job_unit(&s);
  if(rc!=QW_PENDING)return job_finish(&s,rc);
  if(cpu_budget_ms&&s.model->options.now_ms(s.model->options.clock_user)-begin>=cpu_budget_ms)break;
 }
 job_store(&s);return QW_PENDING;
}
int qw_checkpoint_cancel(qw_checkpoint_job *job){
 qw_ck_job_state s;int rc=job_load(job,&s);if(rc)return rc;
 if(!s.progress.active)return s.progress.result;
 s.progress.last_poll_units=0;
 if(!state_ok(s.model))return job_finish(&s,QW_ARGUMENT);
 if(s.model->raw.active){s.progress.failure_reason=QW_UNKNOWN;job_store(&s);return QW_UNKNOWN;}
 if(s.model->execution.active){s.progress.failure_reason=QW_BUSY;job_store(&s);return QW_BUSY;}
 return job_finish(&s,QW_CANCELLED);
}
int qw_checkpoint_job_status(const qw_checkpoint_job *job,qw_checkpoint_progress *out){
 qw_ck_job_state s;int rc=job_load(job,&s);if(rc||!out)return QW_ARGUMENT;
 if(overlaps(job,sizeof *job,out,sizeof *out))return QW_ARGUMENT;
 if(s.progress.active){
  if(overlaps(out,sizeof *out,s.image,s.span)||overlaps(out,sizeof *out,s.model,sizeof *s.model)||
     overlaps(out,sizeof *out,s.arena,s.arena_capacity)||
     (s.work&&overlaps(out,sizeof *out,s.work,sizeof *s.work))||
     overlaps(out,sizeof *out,s.key,sizeof *s.key)||overlaps(out,sizeof *out,s.ids,s.id_bytes))return QW_ARGUMENT;
 }
 *out=s.progress;return QW_OK;
}

