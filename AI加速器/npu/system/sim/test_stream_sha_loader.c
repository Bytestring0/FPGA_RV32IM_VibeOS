/* Concentrated loader contract test. Tiny catalogue cardinalities/embedding
 * vocabulary are fixture-only; no model graph/decode/quality claim is made. */
#include "qw_internal.h"
#undef QW_BASE_W4_TENSORS
#define QW_BASE_W4_TENSORS 1u
#undef QW_CPU_TENSORS
#define QW_CPU_TENSORS 2u
#undef QW_VOCAB
#define QW_VOCAB 2u
#include "qw_load.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "stream_sha_fixtures.h"

static unsigned checks,reset_calls;
static int reset_result;
static qw_model **published;
static int memory_read(void *ctx,uint64_t off,void *out,size_t bytes);
typedef struct {
 const unsigned char *data;size_t size,next,calls;
 int fail_call,short_call;
 npu_storage_source *mutate;
} memory_source;
static int memory_read(void *ctx,uint64_t off,void *out,size_t bytes){
 memory_source *s=ctx;
 assert(bytes&&bytes<=8192&&off==s->next&&off<=s->size&&bytes<=s->size-off);
 if(published)assert(*published==NULL);
 ++s->calls;
 if(s->mutate){s->mutate->read_exact_at=NULL;s->mutate->size_bytes=UINT64_MAX;s->mutate->ctx=NULL;s->mutate=NULL;}
 if(s->fail_call==(int)s->calls)return -7;
 if(s->short_call==(int)s->calls){if(bytes>1)memcpy(out,s->data+(size_t)off,bytes-1);return -9;}
 memcpy(out,s->data+(size_t)off,bytes);s->next+=bytes;return 0;
}
int npu_ag_open(npu_raw_context *c,volatile npu_dma_regs *d,const npu_raw_region *r,size_t n){(void)c;(void)d;(void)r;(void)n;assert(0);return -1;}
int npu_raw_init(npu_raw_context *c,const npu_raw_config *cfg){memset(c,0,sizeof *c);c->config=*cfg;return 0;}
int npu_raw_query(const npu_raw_job *j,const npu_raw_region *r,size_t n,npu_raw_layout *l){(void)j;(void)r;(void)n;(void)l;return -1;}
int qw_reset(qw_model *m){assert(m&&m->magic==QW_MAGIC);if(published)assert(!*published);++reset_calls;return reset_result;}
float qw_bf16(uint16_t bits){(void)bits;return 0;}

static unsigned char payload[131104],destination[131106];
static void source_tests(void){
 for(size_t i=0;i<sizeof payload;i++)payload[i]=(unsigned char)(i*37u+(i>>3));
 for(unsigned i=0;i<sizeof digest_cases/sizeof digest_cases[0];i++){
  size_t n=digest_cases[i].bytes;memory_source memory={payload,n,0,0,0,0,NULL};
  npu_storage_source source={memory_read,&memory,n};memset(destination,0xa5,sizeof destination);
  assert(!load_source(&source,destination+1,digest_cases[i].digest));
  assert(memory.next==n&&memory.calls==(n+8191u)/8192u&&destination[0]==0xa5&&destination[n+1]==0xa5);
  assert(!memcmp(destination+1,payload,n));checks++;
 }
 unsigned char wrong[32];memcpy(wrong,digest_cases[14].digest,32);wrong[0]^=1;
 memory_source m={payload,digest_cases[14].bytes,0,0,0,0,NULL};npu_storage_source source={memory_read,&m,m.size};
 assert(load_source(&source,destination,wrong)==QW_HASH);checks++;
 m=(memory_source){payload,sizeof payload,0,0,2,0,NULL};source=(npu_storage_source){memory_read,&m,m.size};
 assert(load_source(&source,destination,digest_cases[14].digest)==QW_IO&&m.calls==2&&m.next==8192);checks++;
 m=(memory_source){payload,sizeof payload,0,0,0,2,NULL};source=(npu_storage_source){memory_read,&m,m.size};
 assert(load_source(&source,destination,digest_cases[14].digest)==QW_IO&&m.calls==2&&m.next==8192);checks++;
 m=(memory_source){payload,65536,0,0,0,0,NULL};source=(npu_storage_source){memory_read,&m,m.size};m.mutate=&source;
 assert(!load_source(&source,destination,digest_cases[12].digest)&&m.next==65536);checks++;
 load_stream stream;m=(memory_source){payload,64,0,0,0,0,NULL};source=(npu_storage_source){memory_read,&m,64};
 assert(!stream_init(&stream,&source,destination));assert(stream_read(&stream,1,destination,1)==QW_IO&&!m.calls);checks++;
}
static void package_tests(void){
 unsigned char package[sizeof package_data+5],out[sizeof package_data+7];npu_package_view view;
 memcpy(package,package_data,sizeof package_data);memcpy(package+sizeof package_data,"TAIL!",5);
 memory_source m={package,sizeof package,0,0,0,0,NULL};npu_storage_source source={memory_read,&m,m.size};
 memset(out,0xa5,sizeof out);assert(!load_weights(&source,out+1,m.size,package_tail_sha,&view));
 assert(view.bytes==sizeof package_data&&m.next==m.size&&out[0]==0xa5&&out[m.size+1]==0xa5);checks++;
 package[1024]^=1;m=(memory_source){package,sizeof package_data,0,0,0,0,NULL};source=(npu_storage_source){memory_read,&m,m.size};
 assert(load_weights(&source,out,m.size,package_bad_crc_sha,&view)==QW_FORMAT&&!view.buffer);checks++;
 m=(memory_source){package_data,sizeof package_data,0,0,0,1,NULL};source=(npu_storage_source){memory_read,&m,m.size};
 assert(load_weights(&source,out,m.size,package_sha,&view)==QW_FORMAT&&!view.buffer);checks++;
}
static void open_tests(void){
 qw_options options={0};options.context_tokens=8;options.prefill_chunk_tokens=1;options.tile_rows=1;options.arena_ddr_offset=1u<<20;
 options.system_reserved_bytes=1u<<20;options.mode=NPU_RAW_SOFTWARE_REFERENCE;options.weight_sha256=package_sha;options.cpu_sha256=cpu_sha;options.catalog_sha256=catalog_sha;
 qw_memory_plan plan;assert(!qw_plan(&options,sizeof package_data,sizeof cpu_data,sizeof catalog_data,0x3ff00000u,&plan));
 size_t bytes=(plan.arena_bytes+31u)&~(size_t)31;unsigned char *arena=aligned_alloc(32,bytes);assert(arena);
 for(unsigned scenario=0;scenario<8;scenario++){
  memory_source memory[3]={{package_data,sizeof package_data,0,0,0,0,NULL},{cpu_data,sizeof cpu_data,0,0,0,0,NULL},{catalog_data,sizeof catalog_data,0,0,0,0,NULL}};
  npu_storage_source sources[3];for(unsigned i=0;i<3;i++)sources[i]=(npu_storage_source){memory_read,memory+i,memory[i].size};
  qw_model *model=NULL;published=&model;unsigned before=reset_calls;reset_result=0;unsigned char wrong[32];
  options.weight_sha256=package_sha;options.cpu_sha256=cpu_sha;options.catalog_sha256=catalog_sha;
  if(scenario>=1&&scenario<=3){memcpy(wrong,scenario==1?package_sha:scenario==2?cpu_sha:catalog_sha,32);wrong[31]^=1;
   if(scenario==1)options.weight_sha256=wrong;else if(scenario==2)options.cpu_sha256=wrong;else options.catalog_sha256=wrong;
  }
  if(scenario==4)memory[1].short_call=1;
  if(scenario==5)memory[2].fail_call=1;
  if(scenario==6)memory[0].mutate=sources+1;
  if(scenario==7)reset_result=QW_FORMAT;
  qw_memory_plan result_plan;memset(&result_plan,0xa5,sizeof result_plan);
  int rc=qw_open(&model,arena,bytes,&options,sources,sources+1,sources+2,&result_plan);
  if(scenario==0||scenario==6){assert(!rc&&model&&(unsigned char*)model==arena&&reset_calls==before+1);for(unsigned i=0;i<3;i++)assert(memory[i].next==memory[i].size);}
  else{assert(rc==(scenario<=3?QW_HASH:scenario<=5?QW_IO:QW_FORMAT)&&!model);if(scenario!=7)assert(reset_calls==before);}
  checks++;
 }
 published=NULL;qw_model *leased=(qw_model*)arena;memset(leased,0,sizeof *leased);leased->magic=QW_MAGIC;leased->raw.active=1;
 qw_model *original=leased;uint64_t marker=leased->raw.active;assert(qw_open(&leased,NULL,0,NULL,NULL,NULL,NULL,NULL)==QW_UNKNOWN&&leased==original&&leased->raw.active==marker);checks++;
 free(arena);
}
int main(void){source_tests();package_tests();open_tests();printf("PASS_STREAM_SHA_LOADER checks=%u sha_context=%zu no_decode=1 synthetic_catalog=1\n",checks,sizeof(qw_sha256));return 0;}
