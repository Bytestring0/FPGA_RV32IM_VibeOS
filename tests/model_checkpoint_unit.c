#include "qw_internal.h"
#include "qw_sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
 const unsigned char *image;
 size_t bytes,next_offset;
 unsigned calls,fail_call;
 unsigned char *output;
} checkpoint_source_fixture;

static void sha_bytes(const void *data,size_t bytes,unsigned char hash[32]){
 qw_sha256 sha;qw_sha_init(&sha);qw_sha_update(&sha,data,bytes);qw_sha_final(&sha,hash);
}
static void model_fingerprint(const qw_model *m,unsigned char hash[32]){
 /* Integrity-only digest; no pointer-bearing model context is restored/copied. */
 qw_sha256 sha;qw_sha_init(&sha);qw_sha_update(&sha,m,sizeof *m);
 qw_sha_update(&sha,m->arena,m->arena_capacity);
 if(m->w)qw_sha_update(&sha,m->w,sizeof *m->w);
 qw_sha_final(&sha,hash);
}
static void assert_model_same(const qw_model *m,const unsigned char expected[32]){
 unsigned char actual[32];model_fingerprint(m,actual);assert(!memcmp(actual,expected,32));
}
static int source_read(void *context,uint64_t offset,void *destination,size_t bytes){
 checkpoint_source_fixture *f=context;
 assert(offset==f->next_offset&&offset<=f->bytes&&bytes<=f->bytes-(size_t)offset);
 assert(!memcmp(f->output,"\0\0\0\0",4)); /* no partly valid image during I/O */
 ++f->calls;
 if(f->calls==f->fail_call){
  /* A real exact-read failure may already have written a short prefix. */
  memcpy(destination,f->image+(size_t)offset,bytes/2);return -1;
 }
 memcpy(destination,f->image+(size_t)offset,bytes);f->next_offset+=bytes;return 0;
}
static void source_prepare(checkpoint_source_fixture *f,unsigned char *output,
                           size_t bytes,unsigned fail_call){
 f->calls=0;f->next_offset=0;f->fail_call=fail_call;f->output=output;
 memset(output,0xa5,bytes);
}
static void test_checkpoint_source(void){
 qw_model *m=calloc(1,sizeof *m);assert(m);
 m->arena_capacity=12288;m->arena=malloc(m->arena_capacity);assert(m->arena);
 memset(m->arena,0x55,m->arena_capacity);m->plan.state_offset=64;m->plan.state_bytes=8192;
 m->w=malloc(sizeof *m->w);assert(m->w);memset(m->w,0x66,sizeof *m->w);
 m->magic=QW_MAGIC;m->options.context_tokens=8;m->position=2;m->npu_jobs=17;
 m->processed_ids[0]=41;m->processed_ids[1]=42;
 for(size_t i=0;i<m->plan.state_bytes;i++)m->arena[64+i]=(unsigned char)(i*13u+7u);
 for(unsigned i=0;i<96;i++)m->parameter_sha256[i]=(unsigned char)i;
 uint32_t ids[2]={41,42};qw_checkpoint_key key={0};
 key.prefix_ids=ids;key.prefix_count=2;key.tokenizer_sha256[0]=1;
 key.template_sha256[0]=2;key.schema_sha256[0]=3;
 size_t bytes=0;assert(qw_checkpoint_bytes(m,2,&bytes)==QW_OK&&bytes==8456);
 unsigned char *image=malloc(bytes),*output=malloc(bytes),*damaged=malloc(bytes);
 assert(image&&output&&damaged);assert(qw_checkpoint_save(m,&key,image,bytes)==QW_OK);
 unsigned char expected[32],original[32],bad_hash[32];sha_bytes(image,bytes,expected);
 memset(m->arena+64,0x19,m->plan.state_bytes);m->position=1;m->needs_reset=1;
 m->logits_valid=1;m->last_error=QW_DEVICE;m->processed_ids[2]=999;
 m->execution.last_poll_ms=77;
 model_fingerprint(m,original);
 checkpoint_source_fixture f={image,bytes,0,0,0,output};
 npu_storage_source source={source_read,&f,(uint64_t)bytes};
 source_prepare(&f,output,bytes,0);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes-1)==QW_CAPACITY);
 assert(!f.calls&&output[0]==0xa5);assert_model_same(m,original);
 source.size_bytes=bytes-1;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_FORMAT);
 source.size_bytes=bytes+1;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_FORMAT);
 source.size_bytes=UINT64_MAX;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_FORMAT);
 source.size_bytes=bytes;assert(!f.calls&&output[0]==0xa5);assert_model_same(m,original);
 size_t saved_capacity=m->arena_capacity,saved_offset=m->plan.state_offset;
 size_t saved_state_bytes=m->plan.state_bytes;
 m->arena_capacity=SIZE_MAX;m->plan.state_offset=0;m->plan.state_bytes=SIZE_MAX-128u;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_CAPACITY);
 m->arena_capacity=saved_capacity;m->plan.state_offset=saved_offset;m->plan.state_bytes=saved_state_bytes;
 assert(!f.calls&&output[0]==0xa5);assert_model_same(m,original);
 source.read_exact_at=NULL;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_ARGUMENT);
 source.read_exact_at=source_read;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,NULL,output,bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,NULL,bytes)==QW_ARGUMENT);
 assert(!f.calls&&output[0]==0xa5);assert_model_same(m,original);
 /* Alias and pointer-range rejection precede all writes and all callbacks. */
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,m->arena,bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,m,bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,m->w,bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,&key,bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,ids,bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,&source,bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,expected,bytes)==QW_ARGUMENT);
 source.ctx=output+bytes-1;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_ARGUMENT);
 source.ctx=&f;
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,SIZE_MAX)==QW_ARGUMENT);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,(void *)(UINTPTR_MAX-1u),bytes)==QW_ARGUMENT);
 key.prefix_ids=(const uint32_t *)(UINTPTR_MAX-1u);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_ARGUMENT);
 key.prefix_ids=ids;assert(!f.calls&&output[0]==0xa5);assert_model_same(m,original);
 /* UNKNOWN/BUSY win over invalid source/key arguments and leave output alone. */
 memcpy(output,image,bytes);
 m->raw.active=1;model_fingerprint(m,original);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_UNKNOWN);
 assert(qw_checkpoint_restore_from_source(m,NULL,NULL,NULL,NULL,0)==QW_UNKNOWN);
 assert(m->raw.active&&!f.calls&&!memcmp(output,image,bytes));assert_model_same(m,original);
 m->raw.active=0;m->execution.active=1;model_fingerprint(m,original);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_BUSY);
 assert(qw_checkpoint_restore_from_source(m,NULL,NULL,NULL,NULL,0)==QW_BUSY);
 assert(m->execution.active&&!f.calls&&!memcmp(output,image,bytes));assert_model_same(m,original);
 m->execution.active=0;model_fingerprint(m,original);
 /* Header + 4096 + 4096 + 8. Every partial-read boundary must fail atomically. */
 for(unsigned fail_call=1;fail_call<=4;fail_call++){
  source_prepare(&f,output,bytes,fail_call);
  assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_IO);
  assert(f.calls==fail_call&&!memcmp(output,"\0\0\0\0",4));assert_model_same(m,original);
 }
 memcpy(bad_hash,expected,32);bad_hash[0]^=1;source_prepare(&f,output,bytes,0);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,bad_hash,output,bytes)==QW_HASH);
 assert(f.calls==4&&!memcmp(output,"\0\0\0\0",4));assert_model_same(m,original);
 /* Authenticated-but-invalid images must still pass existing strict restore. */
 memcpy(damaged,image,bytes);damaged[0]^=1;sha_bytes(damaged,bytes,bad_hash);
 f.image=damaged;source_prepare(&f,output,bytes,0);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,bad_hash,output,bytes)==QW_FORMAT);
 assert(f.calls==4&&!memcmp(output,"\0\0\0\0",4));assert_model_same(m,original);
 memcpy(damaged,image,bytes);damaged[bytes-1]^=1;sha_bytes(damaged,bytes,bad_hash);
 source_prepare(&f,output,bytes,0);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,bad_hash,output,bytes)==QW_HASH);
 assert(!memcmp(output,"\0\0\0\0",4));assert_model_same(m,original);
 memcpy(damaged,image,bytes);damaged[256]^=1;sha_bytes(damaged,bytes,bad_hash);
 source_prepare(&f,output,bytes,0);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,bad_hash,output,bytes)==QW_TOKEN);
 assert(!memcmp(output,"\0\0\0\0",4));assert_model_same(m,original);
 f.image=image;key.schema_sha256[0]^=1;source_prepare(&f,output,bytes,0);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_HASH);
 assert(!memcmp(output,"\0\0\0\0",4));assert_model_same(m,original);key.schema_sha256[0]^=1;
 source_prepare(&f,output,bytes,0);
 assert(qw_checkpoint_restore_from_source(m,&key,&source,expected,output,bytes)==QW_OK);
 assert(f.calls==4&&f.next_offset==bytes&&!memcmp(output,image,bytes));
 assert(m->position==2&&!m->needs_reset&&!m->logits_valid&&!m->last_error&&m->npu_jobs==17);
 assert(m->processed_ids[0]==41&&m->processed_ids[1]==42&&m->processed_ids[2]==0);
 for(size_t i=0;i<m->plan.state_bytes;i++)assert(m->arena[64+i]==(unsigned char)(i*13u+7u));
 for(size_t i=0;i<sizeof *m->w;i++)assert(((unsigned char *)m->w)[i]==0);
 assert(!m->execution.active&&!m->execution.last_poll_ms);
 free(damaged);free(output);free(image);free(m->w);free(m->arena);free(m);
}

typedef struct {
 qw_model *m;
 qw_checkpoint_key key;
 uint32_t *ids;
 unsigned char *image,*output;
 size_t bytes;
} cooperative_fixture;
static void cooperative_setup(cooperative_fixture *f){
 memset(f,0,sizeof *f);f->m=calloc(1,sizeof *f->m);assert(f->m);
 qw_model *m=f->m;m->arena_capacity=20000;m->arena=malloc(m->arena_capacity);assert(m->arena);
 memset(m->arena,0x5a,m->arena_capacity);m->plan.state_offset=64;m->plan.state_bytes=12289;
 m->w=malloc(sizeof *m->w);assert(m->w);memset(m->w,0x33,sizeof *m->w);
 m->magic=QW_MAGIC;m->options.context_tokens=QW_CONTEXT_MAX;m->position=1536;m->logits_valid=1;
 f->ids=malloc(1536*sizeof *f->ids);assert(f->ids);
 for(size_t i=0;i<1536;i++){f->ids[i]=(uint32_t)(41+i);m->processed_ids[i]=f->ids[i];}
 for(size_t i=0;i<m->plan.state_bytes;i++)m->arena[64+i]=(unsigned char)(i*7u+9u);
 for(unsigned i=0;i<96;i++)m->parameter_sha256[i]=(unsigned char)i;
 f->key.prefix_ids=f->ids;f->key.prefix_count=1536;
 f->key.tokenizer_sha256[0]=3;f->key.template_sha256[0]=4;f->key.schema_sha256[0]=5;
 assert(qw_checkpoint_bytes(m,1536,&f->bytes)==QW_OK);
 f->image=malloc(f->bytes);f->output=malloc(f->bytes);assert(f->image&&f->output);
 assert(qw_checkpoint_save(m,&f->key,f->image,f->bytes)==QW_OK);
 memset(f->output,0xa5,f->bytes);
}
static void cooperative_free(cooperative_fixture *f){
 assert(!f->m->checkpoint_active&&!f->m->checkpoint_job);
 free(f->output);free(f->image);free(f->ids);free(f->m->w);free(f->m->arena);free(f->m);
}
static void cooperative_data_hash(const qw_model *m,unsigned char hash[32]){
 /* Excludes only transaction ownership bookkeeping; all state stays covered. */
 qw_sha256 sha;qw_sha_init(&sha);qw_sha_update(&sha,m->arena,m->arena_capacity);
 qw_sha_update(&sha,m->w,sizeof *m->w);qw_sha_update(&sha,m->processed_ids,sizeof m->processed_ids);
 qw_sha_update(&sha,&m->position,sizeof m->position);qw_sha_update(&sha,&m->needs_reset,sizeof m->needs_reset);
 qw_sha_update(&sha,&m->logits_valid,sizeof m->logits_valid);qw_sha_update(&sha,&m->last_error,sizeof m->last_error);
 qw_sha_update(&sha,&m->execution,sizeof m->execution);qw_sha_final(&sha,hash);
}
static void assert_data_same(const qw_model *m,const unsigned char expected[32]){
 unsigned char actual[32];cooperative_data_hash(m,actual);assert(!memcmp(actual,expected,32));
}
static int cooperative_step(qw_checkpoint_job *job,uint32_t quota,uint32_t budget){
 qw_checkpoint_progress before,after;
 assert(qw_checkpoint_job_status(job,&before)==QW_OK);
 int rc=qw_checkpoint_poll(job,quota,budget);
 assert(qw_checkpoint_job_status(job,&after)==QW_OK);
 assert(after.last_poll_units<=quota);
 assert(after.hash_bytes>=before.hash_bytes&&after.copy_bytes>=before.copy_bytes&&after.clear_bytes>=before.clear_bytes);
 assert(after.hash_bytes-before.hash_bytes<=(size_t)quota*QW_CHECKPOINT_BLOCK_BYTES);
 assert(after.copy_bytes-before.copy_bytes+after.clear_bytes-before.clear_bytes<=
        (size_t)quota*QW_CHECKPOINT_BLOCK_BYTES);
 return rc;
}
static int cooperative_finish(qw_checkpoint_job *job){
 int rc=QW_PENDING;unsigned polls=0;
 while(rc==QW_PENDING){rc=cooperative_step(job,1,0);assert(++polls<2000);}
 return rc;
}
static void cooperative_reach_copy(qw_checkpoint_job *job){
 qw_checkpoint_progress p;unsigned polls=0;
 do{
  assert(qw_checkpoint_job_status(job,&p)==QW_OK);
  if(p.phase==QW_CHECKPOINT_RESTORE_STATE)break;
  assert(cooperative_step(job,1,0)==QW_PENDING);assert(++polls<2000);
 }while(1);
 assert(!p.state_modified&&p.copy_bytes==0);
}
static uint64_t cooperative_clock(void *context){uint64_t *ticks=context;return (*ticks)++;}
static void test_cooperative_save_restore_reset(void){
 _Static_assert(_Alignof(qw_checkpoint_job)>=_Alignof(max_align_t),"job alignment");
 cooperative_fixture f;cooperative_setup(&f);qw_model *m=f.m;
 qw_checkpoint_job job={0},other={0};qw_checkpoint_progress p;
 unsigned char unchanged[32];cooperative_data_hash(m,unchanged);
 assert(qw_checkpoint_begin_save(&job,m,&f.key,f.output,f.bytes)==QW_OK);
 assert(m->checkpoint_active&&m->checkpoint_job==&job);assert_data_same(m,unchanged);
 assert(!memcmp(f.output,"\0\0\0\0",4)&&f.output[256]==0xa5);
 assert(qw_checkpoint_begin_save(&other,m,&f.key,f.output,f.bytes)==QW_BUSY);
 assert(qw_checkpoint_save(m,&f.key,f.output,f.bytes)==QW_BUSY);
 assert(qw_checkpoint_restore(m,&f.key,f.image,f.bytes)==QW_BUSY);
 assert(qw_checkpoint_restore_from_source(m,NULL,NULL,NULL,NULL,0)==QW_BUSY);
 assert(qw_checkpoint_poll(&job,0,0)==QW_ARGUMENT);
 assert(qw_checkpoint_poll(&job,9,1)==QW_ARGUMENT); /* budget requires clock */
 assert(m->checkpoint_active);assert(qw_checkpoint_job_status(&job,&p)==QW_OK&&p.last_poll_units==0);
 assert(cooperative_step(&job,1,0)==QW_PENDING);
 assert(qw_checkpoint_job_status(&job,&p)==QW_OK&&p.hash_bytes==4096&&p.copy_bytes==4096);
 uint64_t ticks=0;m->options.now_ms=cooperative_clock;m->options.clock_user=&ticks;
 assert(cooperative_step(&job,100,1)==QW_PENDING);
 assert(qw_checkpoint_job_status(&job,&p)==QW_OK&&p.last_poll_units==1);
 m->options.now_ms=NULL;m->options.clock_user=NULL;
 while(1){
  assert(qw_checkpoint_job_status(&job,&p)==QW_OK);
  if(!p.active)break;
  assert(!memcmp(f.output,"\0\0\0\0",4));
  int rc=cooperative_step(&job,1,0);assert(rc==QW_OK||rc==QW_PENDING);
  assert_data_same(m,unchanged);
 }
 assert(!memcmp(f.output,f.image,f.bytes)&&!m->checkpoint_active&&!m->checkpoint_job);
 assert(qw_checkpoint_poll(&job,1,0)==QW_OK); /* terminal replay */
 /* Same terminal job is reused directly, never memset to discard ownership. */
 memset(m->arena+64,0x19,m->plan.state_bytes);m->position=1;m->logits_valid=1;
 m->processed_ids[QW_CONTEXT_MAX-1]=999;m->execution.last_poll_ms=77;
 cooperative_data_hash(m,unchanged);
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.output,f.bytes)==QW_OK);
 cooperative_reach_copy(&job);assert_data_same(m,unchanged);assert(m->position==1&&m->logits_valid);
 assert(cooperative_step(&job,1,0)==QW_PENDING);
 assert(m->needs_reset&&!m->logits_valid&&m->position==1&&m->checkpoint_active);
 assert(!memcmp(m->arena+64,f.output+256+1536*4,4096));assert(m->arena[64+4096]==0x19);
 m->raw.active=1;assert(qw_checkpoint_cancel(&job)==QW_UNKNOWN);
 assert(m->checkpoint_active&&m->checkpoint_job==&job&&m->raw.active);
 m->raw.active=0;assert(qw_checkpoint_cancel(&job)==QW_NEED_RESET);
 assert(!m->checkpoint_active&&!m->checkpoint_job&&m->needs_reset&&m->position==1);
 assert(qw_checkpoint_job_status(&job,&p)==QW_OK&&p.failure_reason==QW_CANCELLED&&p.state_modified);
 assert(qw_checkpoint_begin_save(&job,m,&f.key,f.output,f.bytes)==QW_NEED_RESET);
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.output,f.bytes)==QW_OK);
 assert(cooperative_finish(&job)==QW_OK);assert(m->position==1536&&!m->needs_reset&&!m->logits_valid);
 for(size_t i=0;i<m->plan.state_bytes;i++)assert(m->arena[64+i]==(unsigned char)(i*7u+9u));
 for(size_t i=0;i<1536;i++)assert(m->processed_ids[i]==f.ids[i]);
 for(size_t i=1536;i<QW_CONTEXT_MAX;i++)assert(!m->processed_ids[i]);
 for(size_t i=0;i<sizeof *m->w;i++)assert(!((unsigned char *)m->w)[i]);
 assert(!m->execution.last_poll_ms);
 cooperative_data_hash(m,unchanged);
 assert(qw_checkpoint_begin_reset(&job,m)==QW_OK);
 assert(qw_checkpoint_cancel(&job)==QW_CANCELLED);assert_data_same(m,unchanged);
 assert(qw_checkpoint_begin_reset(&job,m)==QW_OK);
 assert(cooperative_step(&job,1,0)==QW_PENDING);assert(m->needs_reset&&m->position==1536);
 for(size_t i=0;i<4096;i++)assert(m->arena[64+i]==0);
 assert(m->arena[64+4096]==(unsigned char)(4096*7u+9u));
 assert(qw_checkpoint_cancel(&job)==QW_NEED_RESET);
 assert(qw_checkpoint_begin_reset(&job,m)==QW_OK);assert(cooperative_finish(&job)==QW_OK);
 assert(!m->position&&!m->needs_reset&&!m->logits_valid&&!m->last_error);
 for(size_t i=0;i<m->plan.state_bytes;i++)assert(!m->arena[64+i]);
 for(size_t i=0;i<QW_CONTEXT_MAX;i++)assert(!m->processed_ids[i]);
 assert(m->arena[0]==0x5a&&m->arena[64+m->plan.state_bytes]==0x5a);
 cooperative_free(&f);
}
static void test_cooperative_failures(void){
 cooperative_fixture f;cooperative_setup(&f);qw_model *m=f.m;
 qw_checkpoint_job job={0};qw_checkpoint_progress p;
 unsigned char unchanged[32],job_before[32],job_after[32];cooperative_data_hash(m,unchanged);
 sha_bytes(&job,sizeof job,job_before);m->raw.active=1;
 assert(qw_checkpoint_begin_save(&job,m,&f.key,f.output,f.bytes)==QW_UNKNOWN);
 assert(qw_checkpoint_begin_restore(&job,m,NULL,NULL,0)==QW_UNKNOWN);
 assert(qw_checkpoint_begin_reset(&job,m)==QW_UNKNOWN);
 sha_bytes(&job,sizeof job,job_after);assert(!memcmp(job_before,job_after,32));
 assert(f.output[0]==0xa5&&!m->checkpoint_active);m->raw.active=0;assert_data_same(m,unchanged);
 qw_checkpoint_job *wrapped=(qw_checkpoint_job *)(UINTPTR_MAX-15u);
 qw_checkpoint_job *unaligned=(qw_checkpoint_job *)((unsigned char *)&job+1);
 assert(qw_checkpoint_begin_save(wrapped,m,&f.key,f.output,f.bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_begin_reset(wrapped,m)==QW_ARGUMENT);
 assert(qw_checkpoint_poll(wrapped,1,0)==QW_ARGUMENT&&qw_checkpoint_cancel(wrapped)==QW_ARGUMENT);
 assert(qw_checkpoint_job_status(wrapped,&p)==QW_ARGUMENT);
 assert(qw_checkpoint_begin_restore(unaligned,m,&f.key,f.image,f.bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_poll(unaligned,1,0)==QW_ARGUMENT);
 assert(qw_checkpoint_begin_save(&job,m,&f.key,m->arena,f.bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_begin_save((qw_checkpoint_job *)m->arena,m,&f.key,f.output,f.bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_begin_save(&job,m,&f.key,&job,f.bytes)==QW_ARGUMENT);
 assert(qw_checkpoint_begin_save(&job,m,&f.key,f.output,SIZE_MAX)==QW_ARGUMENT);
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes-1)==QW_FORMAT);
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes+1)==QW_FORMAT);
 assert_data_same(m,unchanged);assert(!m->checkpoint_active);
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes)==QW_OK);
 assert(qw_checkpoint_job_status(&job,(qw_checkpoint_progress *)f.image)==QW_ARGUMENT);
 m->raw.active=1;assert(qw_checkpoint_poll(&job,1,0)==QW_UNKNOWN);assert(m->checkpoint_active);
 assert(qw_checkpoint_cancel(&job)==QW_UNKNOWN);m->raw.active=0;
 m->execution.active=1;assert(qw_checkpoint_poll(&job,1,0)==QW_BUSY);
 assert(qw_checkpoint_cancel(&job)==QW_BUSY);m->execution.active=0;
 assert(qw_checkpoint_cancel(&job)==QW_CANCELLED);assert_data_same(m,unchanged);
 /* Borrowed identity changes fail before state publication. */
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes)==QW_OK);
 f.key.schema_sha256[0]^=1;
 assert(cooperative_step(&job,1,0)==QW_HASH);f.key.schema_sha256[0]^=1;
 assert_data_same(m,unchanged);assert(!m->checkpoint_active);
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes)==QW_OK);
 f.ids[0]^=1;assert(cooperative_step(&job,1,0)==QW_TOKEN);f.ids[0]^=1;assert_data_same(m,unchanged);
 f.image[f.bytes-1]^=1;
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes)==QW_OK);
 assert(cooperative_finish(&job)==QW_HASH);f.image[f.bytes-1]^=1;assert_data_same(m,unchanged);
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes)==QW_OK);
 f.image[0]^=1;assert(cooperative_step(&job,1,0)==QW_FORMAT);f.image[0]^=1;assert_data_same(m,unchanged);
 assert(qw_checkpoint_begin_save(&job,m,&f.key,f.output,f.bytes)==QW_OK);
 assert(cooperative_step(&job,1,0)==QW_PENDING);assert(qw_checkpoint_cancel(&job)==QW_CANCELLED);
 assert(!memcmp(f.output,"\0\0\0\0",4));assert_data_same(m,unchanged);
 /* Rechecked late key mutation after copy cannot claim the old state survived. */
 memset(m->arena+64,0x19,m->plan.state_bytes);m->position=1;m->logits_valid=1;
 assert(qw_checkpoint_begin_restore(&job,m,&f.key,f.image,f.bytes)==QW_OK);
 cooperative_reach_copy(&job);assert(cooperative_step(&job,1,0)==QW_PENDING);
 f.key.schema_sha256[0]^=1;assert(cooperative_step(&job,1,0)==QW_NEED_RESET);f.key.schema_sha256[0]^=1;
 assert(m->needs_reset&&!m->checkpoint_active&&!m->logits_valid);
 assert(qw_checkpoint_job_status(&job,&p)==QW_OK&&p.failure_reason==QW_HASH);
 assert(qw_checkpoint_begin_reset(&job,m)==QW_OK);assert(cooperative_finish(&job)==QW_OK);
 cooperative_free(&f);
}

int main(void){
 qw_model *m=calloc(1,sizeof *m);assert(m);
 m->arena=calloc(1,512);assert(m->arena);m->arena_capacity=512;
 m->plan.state_offset=64;m->plan.state_bytes=96;
 m->options.context_tokens=8;m->magic=QW_MAGIC;m->position=2;
 m->processed_ids[0]=41;m->processed_ids[1]=42;
 for(unsigned i=0;i<96;i++){m->parameter_sha256[i]=(unsigned char)i;
  m->arena[64+i]=(unsigned char)(0x80+i);}
 uint32_t ids[2]={41,42};qw_checkpoint_key key={0};
 for(unsigned i=0;i<32;i++){key.tokenizer_sha256[i]=(uint8_t)i;
  key.template_sha256[i]=(uint8_t)(i+32);key.schema_sha256[i]=(uint8_t)(i+64);}
 key.prefix_ids=ids;key.prefix_count=2;size_t bytes=0;
 assert(qw_checkpoint_bytes(m,2,&bytes)==QW_OK&&bytes==360);
 unsigned char *image=malloc(bytes);assert(image);
 assert(qw_checkpoint_save(m,&key,image,bytes)==QW_OK);
 assert(qw_checkpoint_save(m,&key,m->arena+100,bytes)==QW_ARGUMENT);
 ids[1]=99;assert(qw_checkpoint_save(m,&key,image,bytes)==QW_TOKEN);ids[1]=42;
 memset(m->arena+64,0,96);m->position=1;m->processed_ids[2]=999;
 key.schema_sha256[0]^=1;
 assert(qw_checkpoint_restore(m,&key,image,bytes)==QW_HASH&&m->position==1);
 key.schema_sha256[0]^=1;
 m->parameter_sha256[0]^=1;
 assert(qw_checkpoint_restore(m,&key,image,bytes)==QW_HASH&&m->position==1);
 m->parameter_sha256[0]^=1;
 image[bytes-1]^=1;
 assert(qw_checkpoint_restore(m,&key,image,bytes)==QW_HASH&&m->position==1);
 image[bytes-1]^=1;ids[0]=88;
 assert(qw_checkpoint_restore(m,&key,image,bytes)==QW_TOKEN&&m->position==1);ids[0]=41;
 m->raw.active=1;
 assert(qw_checkpoint_restore(m,&key,image,bytes)==QW_UNKNOWN&&m->position==1);
 m->raw.active=0;m->needs_reset=1;m->logits_valid=1;
 assert(qw_checkpoint_restore(m,&key,image,bytes)==QW_OK);
 assert(m->position==2&&!m->needs_reset&&!m->logits_valid);
 assert(m->processed_ids[0]==41&&m->processed_ids[1]==42&&m->processed_ids[2]==0);
 for(unsigned i=0;i<96;i++)assert(m->arena[64+i]==(unsigned char)(0x80+i));
 assert(qw_checkpoint_invalidate(image,bytes)==QW_OK);
 assert(qw_checkpoint_restore(m,&key,image,bytes)==QW_FORMAT);
 free(image);free(m->arena);free(m);
 test_checkpoint_source();
 test_cooperative_save_restore_reset();test_cooperative_failures();
 puts("MODEL CHECKPOINT UNIT PASS: exact IDs, SHA, source IO, cooperative quota/budget/save/restore/reset/cancel/lease");
 return 0;
}
