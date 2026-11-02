#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../integration/agent_runtime_20261101/sw/model/qw_internal.h"
#include "../integration/agent_runtime_20261101/sw/model/qw_prefix_cache.h"

static int execute(void*u,const npu_tensor_job*j,npu_tensor_stats*s)
{(void)u;(void)j;(void)s;return 0;}
static const uint8_t weight_id[32]={1},cpu_id[32]={2},catalog_id[32]={3};
static void fill_model(qw_model*m,unsigned char*arena,unsigned char*scratch)
{
 memset(m,0,sizeof *m);m->magic=QW_MAGIC;m->position=7;m->arena=arena;m->arena_capacity=256;
 m->plan.arena_bytes=256;m->plan.state_offset=64;m->plan.state_bytes=128;
 m->plan.policy_version=QW_POLICY_VERSION;m->plan.weights_bytes=32;m->plan.cpu_bytes=16;m->plan.catalog_bytes=8;
 m->options.context_tokens=32;m->options.mode=NPU_RAW_SOFTWARE_REFERENCE;
 m->options.weight_sha256=weight_id;m->options.cpu_sha256=cpu_id;m->options.catalog_sha256=catalog_id;
 m->raw.config.mode=NPU_RAW_SOFTWARE_REFERENCE;m->tensor_execute=execute;m->tensor_scratch=scratch;
 m->tensor_scratch_bytes=64;m->tensor_ddr=4096;m->tensor_status.enabled=1;
 m->tensor_status.numeric_version=NPU_TENSOR_NUMERIC_VERSION;
}
int main(void)
{
 unsigned char *arena=aligned_alloc(32,256),*scratch=aligned_alloc(32,64),*ck=aligned_alloc(32,320),*other=aligned_alloc(32,256),*other_s=aligned_alloc(32,64);
 assert(arena&&scratch&&ck&&other&&other_s);memset(arena,0xa5,256);memset(scratch,0,64);memset(ck,0xcc,320);
 qw_model model,second;fill_model(&model,arena,scratch);fill_model(&second,other,other_s);
 for(unsigned i=0;i<128;i++)arena[64+i]=(unsigned char)(i*13u+5u);
 size_t size=0;assert(qw_checkpoint_size(&model,&size)==0&&size==320);
 unsigned char expected[128];memcpy(expected,arena+64,sizeof expected);
 assert(qw_checkpoint_save(&model,ck,size)==0);
 assert(!memcmp(ck+192,expected,sizeof expected));
 memset(arena+64,0,128);model.position=12;model.logits_valid=1;
 assert(qw_checkpoint_restore(&model,ck,size)==0);
 assert(model.position==7&&!model.logits_valid&&!memcmp(arena+64,expected,sizeof expected));

 unsigned char snapshot[128];memcpy(snapshot,arena+64,128);
 unsigned char untouched[320];memset(ck,0xcc,sizeof untouched);memcpy(untouched,ck,sizeof untouched);
 assert(qw_checkpoint_save(&model,ck,size-1)==QW_CAPACITY);
 assert(!memcmp(ck,untouched,sizeof untouched));
 assert(qw_checkpoint_save(&model,(unsigned char*)ck+1,size)==QW_ARGUMENT);
 assert(!memcmp(ck,untouched,sizeof untouched));
 assert(qw_checkpoint_save(&model,arena,size)==QW_ARGUMENT);
 assert(!memcmp(arena+64,snapshot,128));
 model.package.buffer=ck;model.package.bytes=(uint32_t)size;assert(qw_checkpoint_save(&model,ck,size)==QW_ARGUMENT);
 model.package.buffer=0;model.package.bytes=0;
 assert(qw_checkpoint_save(&model,ck,size)==0);
 assert(qw_checkpoint_restore(&model,arena,size)==QW_ARGUMENT);
 assert(!memcmp(arena+64,expected,128));
 assert(qw_checkpoint_restore(&second,ck,size)==QW_FORMAT);
 model.options.lut_nonlinear=1;assert(qw_checkpoint_restore(&model,ck,size)==QW_FORMAT);model.options.lut_nonlinear=0;
 model.raw.config.mode=NPU_RAW_HARDWARE;assert(qw_checkpoint_restore(&model,ck,size)==QW_FORMAT);
 model.raw.config.mode=NPU_RAW_SOFTWARE_REFERENCE;
 model.options.cpu_sha256=weight_id;assert(qw_checkpoint_restore(&model,ck,size)==QW_FORMAT);model.options.cpu_sha256=cpu_id;
 second.raw.config.mode=NPU_RAW_HARDWARE;assert(qw_checkpoint_restore(&second,ck,size)==QW_FORMAT);
 unsigned char unchanged[128];memset(unchanged,0x3c,sizeof unchanged);
 model.raw.active=1;memset(arena+64,0x3c,128);assert(qw_checkpoint_save(&model,ck,size)==QW_UNKNOWN);
 assert(!memcmp(arena+64,unchanged,128));model.raw.active=0;
 model.raw.cache_unknown=1;assert(qw_checkpoint_restore(&model,ck,size)==QW_UNKNOWN);model.raw.cache_unknown=0;
 model.tensor_status.unknown=1;assert(qw_checkpoint_save(&model,ck,size)==QW_UNKNOWN);model.tensor_status.unknown=0;
 model.needs_reset=1;assert(qw_checkpoint_restore(&model,ck,size)==QW_NEED_RESET);model.needs_reset=0;

 ck[192+19]^=0x80;memset(arena+64,0x69,128);memcpy(snapshot,arena+64,128);
 assert(qw_checkpoint_restore(&model,ck,size)==QW_HASH);
 assert(!memcmp(arena+64,snapshot,128));
 ck[192+19]^=0x80;
 unsigned char saved[320];memcpy(saved,ck,size);ck[24]^=1;
 int tamper_rc=qw_checkpoint_restore(&model,ck,size);assert(tamper_rc==QW_HASH||tamper_rc==QW_FORMAT);
 memcpy(ck,saved,size);

 qw_prefix_cache cache={0};uint32_t cache_tokens[8]={0},prefix[8]={11,22,33,44,55,66,77};
 unsigned char *prefix_ck=aligned_alloc(32,320);assert(prefix_ck);
 size_t tb=0,cb=0,total=0;assert(qw_prefix_cache_required(&model,8,&tb,&cb,&total)==0&&tb==32&&cb==320&&total==352);
 assert(qw_prefix_cache_bind(&cache,&model,cache_tokens,tb,prefix_ck,cb)==0);
 model.position=7;assert(qw_prefix_cache_prime(&cache,prefix,7)==0&&cache.state==QW_PREFIX_CACHE_VALID);
 unsigned char prefix_state[128];memcpy(prefix_state,arena+64,128);memset(arena+64,0x17,128);model.position=8;
 size_t matched=99;assert(qw_prefix_cache_reuse(&cache,prefix,7,&matched)==0&&matched==7);
 assert(model.position==7&&!memcmp(arena+64,prefix_state,128));
 uint32_t miss[7]={11,22,33,44,55,66,78};memset(arena+64,0x29,128);memcpy(snapshot,arena+64,128);model.position=8;matched=99;
 assert(qw_prefix_cache_reuse(&cache,miss,7,&matched)==QW_PREFIX_MISS&&matched==0&&model.position==8&&!memcmp(arena+64,snapshot,128));
 matched=99;assert(qw_prefix_cache_reuse(&cache,prefix,6,&matched)==QW_PREFIX_MISS&&matched==0&&model.position==8);
 assert(qw_prefix_cache_prime(&cache,prefix,7)==QW_NEED_RESET&&cache.state==QW_PREFIX_CACHE_VALID);
 model.position=7;assert(qw_prefix_cache_prime(&cache,prefix,7)==0);
 model.tensor_status.numeric_version++;matched=99;assert(qw_prefix_cache_reuse(&cache,prefix,7,&matched)==QW_FORMAT&&matched==0);model.tensor_status.numeric_version--;
 prefix_ck[192+9]^=1;assert(qw_prefix_cache_reuse(&cache,prefix,7,&matched)==QW_HASH&&matched==0);prefix_ck[192+9]^=1;
 model.tensor_status.unknown=1;assert(qw_prefix_cache_invalidate(&cache)==QW_UNKNOWN&&cache.state==QW_PREFIX_CACHE_VALID);
 assert(qw_prefix_cache_bind(&cache,&model,cache_tokens,tb,prefix_ck,cb)==QW_UNKNOWN);
 model.tensor_status.unknown=0;
 /* Metadata alias rejection must precede every write, including UNKNOWN. */
 model.position=7;assert(qw_prefix_cache_prime(&cache,prefix,7)==0);
 qw_model model_before=model;qw_prefix_cache cache_before=cache;
 unsigned char checkpoint_before[320];memcpy(checkpoint_before,prefix_ck,320);
 uint32_t tokens_before[8];memcpy(tokens_before,cache_tokens,sizeof tokens_before);
 size_t *aliases[]={(size_t*)&model.position,(size_t*)arena,(size_t*)scratch,
                    (size_t*)&cache.token_count,(size_t*)cache_tokens,(size_t*)prefix_ck,(size_t*)prefix};
 for(unsigned i=0;i<sizeof aliases/sizeof aliases[0];i++){
  assert(qw_prefix_cache_reuse(&cache,prefix,7,aliases[i])==QW_ARGUMENT);
  assert(!memcmp(&model,&model_before,sizeof model)&&!memcmp(&cache,&cache_before,sizeof cache));
  assert(!memcmp(prefix_ck,checkpoint_before,320)&&!memcmp(cache_tokens,tokens_before,sizeof tokens_before));
 }
 model.tensor_status.unknown=1;model_before=model;matched=99;
 assert(qw_prefix_cache_reuse(&cache,prefix,7,(size_t*)&model.position)==QW_UNKNOWN);
 assert(qw_prefix_cache_reuse(&cache,prefix,7,&matched)==QW_UNKNOWN&&matched==99);
 assert(!memcmp(&model,&model_before,sizeof model));model.tensor_status.unknown=0;
 assert(qw_checkpoint_size(&model,(size_t*)&model.position)==QW_ARGUMENT&&model.position==7);
 assert(qw_checkpoint_size(&model,(size_t*)arena)==QW_ARGUMENT);
 assert(qw_prefix_cache_prime(&cache,(const uint32_t*)prefix_ck,7)==QW_ARGUMENT);
 assert(qw_prefix_cache_prime(&cache,cache_tokens+1,7)==QW_ARGUMENT);
 assert(!memcmp(prefix_ck,checkpoint_before,320)&&!memcmp(cache_tokens,tokens_before,sizeof tokens_before));
 assert(qw_prefix_cache_prime(&cache,cache_tokens,7)==0);
 qw_prefix_cache *in_package=aligned_alloc(32,96);assert(in_package);memset(in_package,0,96);
 model.package.buffer=(unsigned char*)in_package;model.package.bytes=96;
 assert(qw_prefix_cache_bind(in_package,&model,cache_tokens,tb,prefix_ck,cb)==QW_ARGUMENT);
 assert(in_package->magic==0);model.package.buffer=0;model.package.bytes=0;free(in_package);
 assert(qw_prefix_cache_invalidate(&cache)==0&&cache.state==QW_PREFIX_CACHE_BOUND);
 free(prefix_ck);
 free(other_s);free(other);free(ck);free(scratch);free(arena);
 puts("PASS_QW_CHECKPOINT full_state identity capacity alias unknown checksum prefix_metadata_alias");return 0;
}
