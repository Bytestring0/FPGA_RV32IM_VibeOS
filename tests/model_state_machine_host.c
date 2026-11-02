/* Real W4+BF16 model state scheduler/checkpoint differential, not a heldout task. */
#define _POSIX_C_SOURCE 200809L
#include "qw_internal.h"
#include "qw_sha256.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>

typedef struct {FILE *f;uint64_t bytes;} file_source;
static int read_at(void *ctx,uint64_t offset,void *dst,size_t n){
 file_source *s=(file_source *)ctx;
 if(offset>s->bytes||n>s->bytes-offset||fseeko(s->f,(off_t)offset,SEEK_SET))return -1;
 return fread(dst,1,n,s->f)==n?0:-1;
}
static npu_storage_source open_source(const char *path,file_source *s){
 s->f=fopen(path,"rb");if(!s->f||fseeko(s->f,0,SEEK_END))abort();
 off_t size=ftello(s->f);if(size<0)abort();s->bytes=(uint64_t)size;
 return (npu_storage_source){read_at,s,s->bytes};
}
static unsigned hex(char c){return c>='0'&&c<='9'?(unsigned)(c-'0'):(unsigned)(c-'a'+10);}
static void sha_text(const char *text,uint8_t out[32]){
 for(unsigned i=0;i<32;i++)out[i]=(uint8_t)(hex(text[2*i])*16+hex(text[2*i+1]));
}
static void sha_bytes(const void *p,size_t n,uint8_t out[32]){
 qw_sha256 sha;qw_sha_init(&sha);qw_sha_update(&sha,p,n);qw_sha_final(&sha,out);
}
static void print_sha(const uint8_t p[32]){for(unsigned i=0;i<32;i++)printf("%02x",p[i]);}
static FILE *trace_output=0,*trace_raw=0,*trace_index=0;
static uint64_t trace_ordinal=0;
static const char *sample_id="smoke5";
static unsigned delta_head_index[QW_LAYERS]={0};
static void trace_range(void *unused,uint32_t pos,uint32_t layer,const char *stage,
                        const float *data,size_t n){
 (void)unused;if(!trace_output)return;
 if(strcmp(stage,"linear.delta.decay")==0&&layer<QW_LAYERS)delta_head_index[layer]++;
 double min=INFINITY,max=-INFINITY,squares=0;size_t finite=0,nonfinite=0;
 for(size_t i=0;i<n;i++){double x=data[i];if(!isfinite(x)){nonfinite++;continue;}
  if(x<min)min=x;if(x>max)max=x;squares+=x*x;finite++;}
 if(!trace_raw||!trace_index)abort();
 off_t offset=ftello(trace_raw);if(offset<0||fwrite(data,sizeof(float),n,trace_raw)!=n)abort();
 uint8_t digest[32];sha_bytes(data,n*sizeof(float),digest);
  uint64_t ordinal=trace_ordinal++;unsigned indexed_layer=layer==UINT32_MAX?1024u:layer;
 fprintf(trace_index,"{\"sample_id\":\"%s\",\"token\":%u,\"layer\":%u,\"head_seq\":%" PRIu64 ",\"ordinal\":%" PRIu64 ",\"tag\":\"%s\",\"shape\":[%zu],\"dtype\":\"f32le\",\"offset_bytes\":%" PRIu64 ",\"bytes\":%zu,\"sha256\":\"",sample_id,pos,indexed_layer,ordinal,ordinal,stage,n,(uint64_t)offset,n*sizeof(float));
 for(unsigned i=0;i<32;i++)fprintf(trace_index,"%02x",digest[i]);
 fprintf(trace_index,"\"}\n");
 fprintf(trace_output,"{\"sample_id\":\"%s\",\"position\":%u,\"layer\":%u,\"head_seq\":%u,\"stage\":\"%s\",\"elements\":%zu,\"finite\":%zu,\"nonfinite\":%zu,\"min\":%.9g,\"max\":%.9g,\"sumsq\":%.17g}\n",sample_id,pos,layer,layer<QW_LAYERS&&delta_head_index[layer]?(delta_head_index[layer]-1)%16:0,stage,n,finite,nonfinite,finite?min:0,finite?max:0,squares);
}
static uint64_t now_ms(void *unused){
 (void)unused;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))abort();
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
int main(int argc,char **argv){
 if(argc!=4){fprintf(stderr,"usage: model_state_machine_host QWENW4.BIN cpu_raw_tensors.bin TENSOR.JSON\n");return 2;}
 static const char *ids[3]={
  "f2299b56becb55c0f4bf06e38d982055b893c0d8e70d369f97f84fab68ef4141",
  "168f02aee67afee5b56edb8683c98f5fa97dae5c48dad720984a4c1ff2e5262d",
  "400952e58eb87cc2da45e46d3adfd66035473771eb584fff993e9f17083f0f3d"};
 file_source sources[3];npu_storage_source src[3];uint8_t sha[3][32];
 for(int i=0;i<3;i++){src[i]=open_source(argv[i+1],&sources[i]);sha_text(ids[i],sha[i]);}
 qw_options o={0};o.context_tokens=2048;o.prefill_chunk_tokens=128;o.tile_rows=512;
 o.arena_ddr_offset=1u<<20;o.system_reserved_bytes=1u<<20;
 o.mode=NPU_RAW_SOFTWARE_REFERENCE;o.budgets=(npu_raw_budgets){10000000,1000000};
 o.weight_sha256=sha[0];o.cpu_sha256=sha[1];o.catalog_sha256=sha[2];
 o.now_ms=now_ms;o.trace=trace_range;
 size_t max_cap=NPU_RAW_DDR_LIMIT-o.arena_ddr_offset;qw_memory_plan p;
 int rc=qw_plan(&o,src[0].size_bytes,src[1].size_bytes,src[2].size_bytes,max_cap,&p);
 if(rc){fprintf(stderr,"qw_plan %d\n",rc);return 1;}
 void *arena=aligned_alloc(32,p.arena_bytes);if(!arena)return 1;qw_model *m=0;
 uint64_t started=now_ms(0);
 rc=qw_open(&m,arena,p.arena_bytes,&o,&src[0],&src[1],&src[2],&p);
 if(rc){fprintf(stderr,"qw_open %d\n",rc);return 1;}
 printf("MODEL_READY_MS=%" PRIu64 " ARENA_BYTES=%zu STATE_BYTES=%zu\n",now_ms(0)-started,p.arena_bytes,p.state_bytes);fflush(stdout);
 uint32_t prefix[QW_CONTEXT_MAX]={16,24,10,17,18},follow=24;size_t prefix_len=5;
 const char *input_path=getenv("MODEL_PREFIX_IDS_BIN");
 if(input_path){FILE *in=fopen(input_path,"rb");uint32_t count=0;
  if(!in||fread(&count,4,1,in)!=1||!count||count>=QW_CONTEXT_MAX||
     fread(prefix,4,count,in)!=count||fgetc(in)!=EOF||fclose(in))abort();
  prefix_len=count;
 }
 const char *trace_path=getenv("CALIBRATION_TRACE");
 const char *raw_path=getenv("CALIBRATION_RAW_BIN");
 const char *index_path=getenv("CALIBRATION_TRACE_INDEX");
 if(!!trace_path!=!!raw_path||!!trace_path!=!!index_path)abort();
 if(trace_path){sample_id=getenv("CALIBRATION_SAMPLE_ID");if(!sample_id||!sample_id[0])abort();
  trace_output=fopen(trace_path,"wb");trace_raw=fopen(raw_path,"wb");trace_index=fopen(index_path,"wb");
  if(!trace_output||!trace_raw||!trace_index)abort();}
 rc=qw_start(m,prefix,prefix_len,trace_output?1:0);assert(rc==QW_OK);
 size_t done=0;const float *logits=0;unsigned polls=0;uint64_t maxpoll=0;
 do{rc=qw_poll(m,32,15,&done,&logits);qw_status s;assert(!qw_get_status(m,&s));
  if(s.last_poll_ms>maxpoll)maxpoll=s.last_poll_ms;assert(s.last_poll_units<=32);
  if(++polls%5000==0){printf("PREFILL_PROGRESS=%zu/%zu POLLS=%u\n",done,prefix_len,polls);fflush(stdout);}
 }while(rc==QW_PENDING);
 assert(rc==QW_OK&&done==prefix_len&&(!!logits==!!trace_output));
  if(trace_output){if(fclose(trace_output)||fclose(trace_raw)||fclose(trace_index))abort();
   trace_output=0;trace_raw=0;trace_index=0;}
 printf("PREFILL_DONE_MS=%" PRIu64 " POLLS=%u MAX_POLL_MS=%" PRIu64 "\n",now_ms(0)-started,polls,maxpoll);fflush(stdout);
 qw_checkpoint_key key={0};for(unsigned i=0;i<32;i++){key.tokenizer_sha256[i]=(uint8_t)i;
  key.template_sha256[i]=(uint8_t)(i+32);key.schema_sha256[i]=(uint8_t)(i+64);}
 key.prefix_ids=prefix;key.prefix_count=prefix_len;size_t checkpoint_bytes=0;
 assert(qw_checkpoint_bytes(m,prefix_len,&checkpoint_bytes)==QW_OK);
 void *checkpoint=malloc(checkpoint_bytes);assert(checkpoint);
 assert(qw_checkpoint_save(m,&key,checkpoint,checkpoint_bytes)==QW_OK);
 uint8_t prefix_state_sha[32],restored_state_sha[32],recomputed_state_sha[32];
 sha_bytes(m->arena+m->plan.state_offset,m->plan.state_bytes,prefix_state_sha);
 printf("CHECKPOINT_BYTES=%zu PREFIX_TOKENS=%zu MODE=SUPPLIED_RAW_PREFIX_NOT_R4_PEAK_MIN_BYTES=%zu\n",
        checkpoint_bytes,prefix_len,p.arena_bytes+checkpoint_bytes);fflush(stdout);
 rc=qw_start(m,&follow,1,1);assert(rc==QW_OK);
 rc=qw_poll(m,1,1,&done,&logits);assert(rc==QW_PENDING&&done==0);
 assert(qw_checkpoint_save(m,&key,checkpoint,checkpoint_bytes)==QW_BUSY);
 assert(qw_cancel(m)==QW_CANCELLED&&m->needs_reset);
 assert(qw_reset(m)==QW_OK);
 assert(qw_checkpoint_restore(m,&key,checkpoint,checkpoint_bytes)==QW_OK);
 assert(m->position==prefix_len);
 sha_bytes(m->arena+m->plan.state_offset,m->plan.state_bytes,restored_state_sha);
 assert(!memcmp(prefix_state_sha,restored_state_sha,32));
 uint8_t from_coop[32],from_ref[32],state_coop[32],state_ref[32];
 rc=qw_start(m,&follow,1,1);assert(rc==QW_OK);polls=0;
 do{rc=qw_poll(m,64,15,&done,&logits);assert(++polls<1000000);}while(rc==QW_PENDING);
 assert(rc==QW_OK&&done==1&&logits);
 sha_bytes(logits,QW_VOCAB*sizeof(float),from_coop);
 sha_bytes(m->arena+m->plan.state_offset,m->plan.state_bytes,state_coop);
 printf("COOPERATIVE_LOGITS_SHA=");print_sha(from_coop);printf(" STATE_SHA=");print_sha(state_coop);puts("");fflush(stdout);
 assert(qw_reset(m)==QW_OK);size_t recomputed=0;
 assert(qw_prefill_reference(m,prefix,prefix_len,&recomputed,0)==QW_OK&&recomputed==prefix_len);
 sha_bytes(m->arena+m->plan.state_offset,m->plan.state_bytes,recomputed_state_sha);
 if(memcmp(prefix_state_sha,recomputed_state_sha,32)){fprintf(stderr,"INDEPENDENT_PREFIX_RECOMPUTE_MISMATCH\n");return 1;}
 printf("INDEPENDENT_REFERENCE_PREFIX_STATE_MATCH tokens=%zu sha256=",prefix_len);print_sha(prefix_state_sha);puts("");fflush(stdout);
 rc=qw_decode_reference(m,follow,&logits);assert(rc==QW_OK&&logits);
 sha_bytes(logits,QW_VOCAB*sizeof(float),from_ref);
 sha_bytes(m->arena+m->plan.state_offset,m->plan.state_bytes,state_ref);
 printf("REFERENCE_LOGITS_SHA=");print_sha(from_ref);printf(" STATE_SHA=");print_sha(state_ref);puts("");fflush(stdout);
 if(memcmp(from_coop,from_ref,32)||memcmp(state_coop,state_ref,32)){fprintf(stderr,"FULL_MODEL_DIFFERENTIAL_MISMATCH\n");return 1;}
 assert(qw_close(m)==QW_OK);free(checkpoint);free(arena);for(unsigned i=0;i<3;i++)fclose(sources[i].f);
 printf("FULL_MODEL_SOFTWARE_STATE_AND_LOGITS_MATCH elapsed_ms=%" PRIu64 "\n",now_ms(0)-started);return 0;
}
