/* Real existing R5 FP32 state: cooperative checkpoint performance/consistency.
 * Opens original W4/CPU once. No prefill/decode/sample, HTTP or board access.
 * Input assets are immutable and identity-pinned; all outputs must be under E. */
#define _POSIX_C_SOURCE 200809L
#include "qw_internal.h"
#include "qw_sha256.h"
#include "qw_tokenizer.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

typedef struct {FILE *file;uint64_t bytes,read_bytes,calls;} file_source;
typedef struct {
 uint64_t polls,units,total_ns,last_ns,max_ns,over_100ms,phase_polls[16],phase_max_ns[16];
} measurements;
static uint64_t now_ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static uint64_t now_ms(void *ignored){(void)ignored;return now_ns()/1000000u;}
static int read_at(void *ctx,uint64_t offset,void *dst,size_t bytes){
 file_source *s=ctx;if(offset>s->bytes||bytes>s->bytes-offset||fseeko(s->file,(off_t)offset,SEEK_SET))return -1;
 ++s->calls;size_t got=fread(dst,1,bytes,s->file);s->read_bytes+=got;return got==bytes?0:-1;
}
static npu_storage_source open_source(const char *path,file_source *s){
 memset(s,0,sizeof *s);s->file=fopen(path,"rb");assert(s->file&&!fseeko(s->file,0,SEEK_END));
 off_t bytes=ftello(s->file);assert(bytes>=0);s->bytes=(uint64_t)bytes;return(npu_storage_source){read_at,s,s->bytes};
}
static void *read_file(const char *path,size_t *bytes){
 FILE *f=fopen(path,"rb");assert(f&&!fseeko(f,0,SEEK_END));off_t n=ftello(f);assert(n>=0&&!fseeko(f,0,SEEK_SET));
 assert((uint64_t)n<SIZE_MAX);unsigned char *out=malloc((size_t)n+1);assert(out);
 assert(fread(out,1,(size_t)n,f)==(size_t)n&&!fclose(f));out[n]=0;*bytes=(size_t)n;return out;
}
static void sha_bytes(const void *data,size_t bytes,uint8_t hash[32]){qw_sha256 sha;qw_sha_init(&sha);qw_sha_update(&sha,data,bytes);qw_sha_final(&sha,hash);}
static int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static void parse_sha(const char *text,uint8_t hash[32]){assert(strlen(text)==64);for(unsigned i=0;i<32;i++){int a=nibble(text[2*i]),b=nibble(text[2*i+1]);assert(a>=0&&b>=0);hash[i]=(uint8_t)(a*16+b);}}
static void hash_hex(const uint8_t hash[32],char out[65]){static const char h[]="0123456789abcdef";for(unsigned i=0;i<32;i++){out[2*i]=h[hash[i]>>4];out[2*i+1]=h[hash[i]&15];}out[64]=0;}
static void pin(const void *data,size_t bytes,size_t wanted_bytes,const char *wanted,const char *name){
 uint8_t actual[32],expected[32];sha_bytes(data,bytes,actual);parse_sha(wanted,expected);
 assert(bytes==wanted_bytes&&!memcmp(actual,expected,32));printf("PIN %s bytes=%zu sha256=%s\n",name,bytes,wanted);
}
static const char *phase_name(uint32_t phase){
 switch(phase){case QW_CHECKPOINT_SAVE_IDS:return "save_ids";case QW_CHECKPOINT_SAVE_STATE:return "save_state";
 case QW_CHECKPOINT_VERIFY_IDS:return "verify_ids";case QW_CHECKPOINT_VERIFY_STATE:return "verify_state";
 case QW_CHECKPOINT_RESTORE_STATE:return "restore_state";case QW_CHECKPOINT_CLEAR_WORK:return "clear_work";
 case QW_CHECKPOINT_PUBLISH_IDS:return "publish_ids";case QW_CHECKPOINT_COMMIT:return "commit";
 case QW_CHECKPOINT_RESET_STATE:return "reset_state";default:return "other";}
}
static void poll_job(qw_checkpoint_job *job,const char *name,const char *outdir,measurements *m){
 char path[2048];assert(snprintf(path,sizeof path,"%s/%s_poll.csv",outdir,name)>0);
 FILE *csv=fopen(path,"wb");assert(csv);fputs("poll,phase,units,elapsed_ns,hash_bytes,copy_bytes,clear_bytes,result\n",csv);
 memset(m,0,sizeof *m);uint64_t total_begin=now_ns();int rc;
 do{
  qw_checkpoint_progress before,after;assert(qw_checkpoint_job_status(job,&before)==QW_OK);
  uint64_t begin=now_ns();rc=qw_checkpoint_poll(job,1,0);uint64_t elapsed=now_ns()-begin;
  assert(qw_checkpoint_job_status(job,&after)==QW_OK);
  assert(rc==QW_OK||rc==QW_PENDING);assert(after.last_poll_units<=1);
  assert(after.hash_bytes-before.hash_bytes<=QW_CHECKPOINT_BLOCK_BYTES);
  assert(after.copy_bytes-before.copy_bytes+after.clear_bytes-before.clear_bytes<=QW_CHECKPOINT_BLOCK_BYTES);
  m->polls++;m->units+=after.last_poll_units;m->last_ns=elapsed;
  if(elapsed>m->max_ns)m->max_ns=elapsed;if(elapsed>100000000u)m->over_100ms++;
  if(before.phase<16){m->phase_polls[before.phase]++;if(elapsed>m->phase_max_ns[before.phase])m->phase_max_ns[before.phase]=elapsed;}
  fprintf(csv,"%" PRIu64 ",%s,%u,%" PRIu64 ",%zu,%zu,%zu,%d\n",m->polls,phase_name(before.phase),after.last_poll_units,elapsed,after.hash_bytes,after.copy_bytes,after.clear_bytes,rc);
 }while(rc==QW_PENDING);
 m->total_ns=now_ns()-total_begin;assert(!fclose(csv));
 printf("MEASURE operation=%s quota=1 polls=%" PRIu64 " units=%" PRIu64 " total_ms=%.6f last_poll_ms=%.6f max_poll_ms=%.6f polls_over_100ms=%" PRIu64 "\n",
  name,m->polls,m->units,m->total_ns/1000000.0,m->last_ns/1000000.0,m->max_ns/1000000.0,m->over_100ms);
 for(unsigned i=0;i<16;i++)if(m->phase_polls[i])printf("PHASE operation=%s phase=%s polls=%" PRIu64 " max_poll_ms=%.6f\n",name,phase_name(i),m->phase_polls[i],m->phase_max_ns[i]/1000000.0);
 fflush(stdout);
}
static uint64_t resource_reads(const file_source sources[3]){return sources[0].read_bytes+sources[1].read_bytes+sources[2].read_bytes;}
static void write_metrics(FILE *f,const char *name,const measurements *m,int comma){
 fprintf(f,"%s\"%s\":{\"polls\":%" PRIu64 ",\"units\":%" PRIu64 ",\"total_ms\":%.9f,\"last_poll_ms\":%.9f,\"max_poll_ms\":%.9f,\"polls_over_100ms\":%" PRIu64 "}",
 comma?",\n":"",name,m->polls,m->units,m->total_ns/1000000.0,m->last_ns/1000000.0,m->max_ns/1000000.0,m->over_100ms);
}
int main(int argc,char **argv){
 if(argc!=10){fprintf(stderr,"usage: model_checkpoint_cooperative_host W4 CPU CATALOG TOKEN R5.txt PREFIX.BIN SCHEMA R5CKPT.BIN E_OUTDIR\n");return 2;}
 const char *allowed_out="/mnt/e/FPGA_RV32IM_Work/deepseek_agent_accel_20261102/checkpoint_cooperative/real_state/";
 assert(!strncmp(argv[9],allowed_out,strlen(allowed_out)));
 const char *model_hash[3]={"f2299b56becb55c0f4bf06e38d982055b893c0d8e70d369f97f84fab68ef4141","168f02aee67afee5b56edb8683c98f5fa97dae5c48dad720984a4c1ff2e5262d","400952e58eb87cc2da45e46d3adfd66035473771eb584fff993e9f17083f0f3d"};
 const uint64_t model_sizes[3]={256604672u,509561536u,560944u};
 file_source files[3];npu_storage_source sources[3];uint8_t hashes[3][32];
 for(unsigned i=0;i<3;i++){sources[i]=open_source(argv[i+1],&files[i]);assert(sources[i].size_bytes==model_sizes[i]);parse_sha(model_hash[i],hashes[i]);}
 size_t token_bytes,text_bytes,ids_bytes,schema_bytes,image_bytes;
 void *token=read_file(argv[4],&token_bytes),*text=read_file(argv[5],&text_bytes);
 void *binary_ids=read_file(argv[6],&ids_bytes),*schema=read_file(argv[7],&schema_bytes);
 /* Read existing checkpoint ONCE, offline, before any warm operation. */
 void *image=read_file(argv[8],&image_bytes);
 pin(token,token_bytes,13291892,"66df043b96ef2ccaa8d6e24148b0dc03eb022e8f461a88fdc527c178cbe4aa45","TOKEN");
 pin(text,text_bytes,6915,"03de523d76e46109477bb22b96e9d6bd2ed976f8ac53c279d79d1da99e7ba0ea","R5_TEXT");
 pin(binary_ids,ids_bytes,6444,"94150057d10439ae3fbf5dc79dbe01a289776af1a2ae4d4ca145ee06b49a5f40","R5_IDS");
 uint8_t schema_hash[32];sha_bytes(schema,schema_bytes,schema_hash);uint8_t schema_expected[32];parse_sha("e2b6f45bd1a760c77a09063fa30a5e114144c5d26ebd2416a9588084f17006e1",schema_expected);assert(!memcmp(schema_hash,schema_expected,32));
 printf("PIN SCHEMA bytes=%zu sha256=e2b6f45bd1a760c77a09063fa30a5e114144c5d26ebd2416a9588084f17006e1\n",schema_bytes);
 pin(image,image_bytes,70539816,"1eeec58a4c4cda427c930110e53a9db14bb71f7b3629e488a6e2e149bb352741","R5_CHECKPOINT");
 uint32_t count;memcpy(&count,binary_ids,4);assert(count==1610&&ids_bytes==4+(size_t)count*4);
 qw_tokenizer tokenizer;uint8_t token_hash[32];sha_bytes(token,token_bytes,token_hash);assert(qt_open(&tokenizer,token,token_bytes,token_hash)==QW_OK);
 size_t workspace_bytes=qt_workspace_bytes(text_bytes);void *workspace=malloc(workspace_bytes);uint32_t *encoded=malloc(QW_CONTEXT_MAX*sizeof *encoded);assert(workspace&&encoded);size_t encoded_count=0;
 assert(qt_encode(&tokenizer,text,text_bytes,1,encoded,QW_CONTEXT_MAX,&encoded_count,workspace,workspace_bytes)==QW_OK);
 assert(encoded_count==count&&!memcmp(encoded,(unsigned char *)binary_ids+4,count*4));
 qw_checkpoint_key key={0};memcpy(key.tokenizer_sha256,token_hash,32);sha_bytes(text,text_bytes,key.template_sha256);memcpy(key.schema_sha256,schema_hash,32);
 key.prefix_ids=encoded;key.prefix_count=count;
 qw_options options={0};options.context_tokens=2048;options.prefill_chunk_tokens=128;options.tile_rows=512;
 options.arena_ddr_offset=1u<<20;options.system_reserved_bytes=1u<<20;options.mode=NPU_RAW_SOFTWARE_REFERENCE;
 options.budgets=(npu_raw_budgets){10000000,1000000};options.weight_sha256=hashes[0];options.cpu_sha256=hashes[1];options.catalog_sha256=hashes[2];options.now_ms=now_ms;
 qw_memory_plan plan;size_t cap=NPU_RAW_DDR_LIMIT-options.arena_ddr_offset;
 assert(qw_plan(&options,sources[0].size_bytes,sources[1].size_bytes,sources[2].size_bytes,cap,&plan)==QW_OK);
 size_t allocation=(plan.arena_bytes+31u)&~(size_t)31u;void *arena=aligned_alloc(32,allocation);assert(arena);qw_model *model=NULL;
 uint64_t open_begin=now_ns();int rc=qw_open(&model,arena,plan.arena_bytes,&options,&sources[0],&sources[1],&sources[2],&plan);
 assert(rc==QW_OK&&model);printf("MODEL_OPEN_ONCE open_ms=%.6f arena_bytes=%zu state_bytes=%zu source_read_bytes=%" PRIu64 "\n",(now_ns()-open_begin)/1000000.0,plan.arena_bytes,plan.state_bytes,resource_reads(files));
 size_t wanted;assert(qw_checkpoint_bytes(model,count,&wanted)==QW_OK&&wanted==image_bytes);
 assert(plan.state_bytes==image_bytes-256-count*4);uint64_t warm_reads=resource_reads(files);
 qw_checkpoint_job job={0};measurements restore,save,reset;qw_status status;
 /* Guard test with an injected UNKNOWN marker: no device is contacted. */
 uint8_t job_before[32],job_after[32];sha_bytes(&job,sizeof job,job_before);model->raw.active=1;
 assert(qw_checkpoint_begin_restore(&job,model,&key,image,image_bytes)==QW_UNKNOWN);
 sha_bytes(&job,sizeof job,job_after);assert(!memcmp(job_before,job_after,32)&&!model->checkpoint_active&&model->raw.active);
 model->raw.active=0;printf("UNKNOWN_GATE_INJECTION_PASS no_device_submission=1 job_unchanged=1\n");
 uint64_t begin=now_ns();assert(qw_checkpoint_begin_restore(&job,model,&key,image,image_bytes)==QW_OK);printf("BEGIN restore_ms=%.6f\n",(now_ns()-begin)/1000000.0);
 poll_job(&job,"restore",argv[9],&restore);assert(qw_get_status(model,&status)==QW_OK&&status.position==1610&&!status.needs_reset&&!status.checkpoint_active&&!status.lease_active);
 uint8_t state_hash[32],image_state_hash[32];char state_hex[65];sha_bytes(model->arena+plan.state_offset,plan.state_bytes,state_hash);
 sha_bytes((unsigned char *)image+256+count*4,plan.state_bytes,image_state_hash);assert(!memcmp(state_hash,image_state_hash,32));hash_hex(state_hash,state_hex);printf("RESTORED position=1610 state_sha256=%s\n",state_hex);
 void *saved=malloc(image_bytes);assert(saved);memset(saved,0xa5,image_bytes);
 begin=now_ns();assert(qw_checkpoint_begin_save(&job,model,&key,saved,image_bytes)==QW_OK);printf("BEGIN save_ms=%.6f\n",(now_ns()-begin)/1000000.0);
 poll_job(&job,"save",argv[9],&save);uint8_t saved_hash[32],original_hash[32];sha_bytes(saved,image_bytes,saved_hash);sha_bytes(image,image_bytes,original_hash);
 assert(!memcmp(saved_hash,original_hash,32)&&!memcmp(saved,image,image_bytes));printf("SAVE_EXACT_R5_IMAGE_PASS bytes=%zu sha256=1eeec58a4c4cda427c930110e53a9db14bb71f7b3629e488a6e2e149bb352741\n",image_bytes);
 char path[2048];assert(snprintf(path,sizeof path,"%s/RESAVED.BIN",argv[9])>0);FILE *out=fopen(path,"wb");assert(out&&fwrite(saved,1,image_bytes,out)==image_bytes&&!fclose(out));
 begin=now_ns();assert(qw_checkpoint_begin_reset(&job,model)==QW_OK);printf("BEGIN reset_ms=%.6f\n",(now_ns()-begin)/1000000.0);
 poll_job(&job,"reset",argv[9],&reset);assert(qw_get_status(model,&status)==QW_OK&&!status.position&&!status.needs_reset&&!status.checkpoint_active&&!status.logits);
 for(size_t i=0;i<plan.state_bytes;i++)assert(model->arena[plan.state_offset+i]==0);
 for(size_t i=0;i<sizeof *model->w;i++)assert(((const unsigned char *)model->w)[i]==0);
 for(size_t i=0;i<QW_CONTEXT_MAX;i++)assert(model->processed_ids[i]==0);
 assert(resource_reads(files)==warm_reads&&!status.npu_jobs);printf("RESET_ZERO_STATE_PASS position=0 state_bytes=%zu MODEL_RESOURCE_WARM_READ_BYTES=0 generated_tokens=0 npu_jobs=0\n",plan.state_bytes);
 assert(snprintf(path,sizeof path,"%s/RESULT.json",argv[9])>0);out=fopen(path,"wb");assert(out);
 fprintf(out,"{\"test\":\"real_r5_fp32_checkpoint_cooperative\",\"position_restored\":1610,\"state_bytes\":%zu,\"state_sha256\":\"%s\",\"image_bytes\":%zu,\"same_r5_blob\":true,\"reset_zero\":true,\"warm_resource_read_bytes\":0,\"generated_tokens\":0,\"board_run\":false,\"timing_includes_per_poll_csv_io_only_in_total\":true,\n",plan.state_bytes,state_hex,image_bytes);
 write_metrics(out,"restore",&restore,0);write_metrics(out,"save",&save,1);write_metrics(out,"reset",&reset,1);fputs("}\n",out);assert(!fclose(out));
 assert(qw_close(model)==QW_OK);free(saved);free(arena);for(unsigned i=0;i<3;i++)assert(!fclose(files[i].file));
 free(workspace);free(encoded);free(image);free(schema);free(binary_ids);free(text);free(token);
 puts("REAL_70MB_COOPERATIVE_CHECKPOINT_PASS_NO_GENERATION_NO_BOARD");return 0;
}
