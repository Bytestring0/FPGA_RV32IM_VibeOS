/* Actual immutable full-model comparison, not a synthetic operator demo.
 * Timing is explicitly HOST SOFTWARE_REFERENCE, never FPGA throughput. */
#define _POSIX_C_SOURCE 200809L
#include "qw_runtime.h"
#include "qw_head.h"
#include "qw_tensor_v2.h"
#include "qw_tokenizer.h"
#include "npu_tensor_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <time.h>
#include <sys/types.h>
typedef struct {FILE *file;uint64_t size;} input_file;
static int read_at(void *v,uint64_t offset,void *dst,size_t bytes) {
 input_file *s=v;
 if(offset>s->size||bytes>s->size-offset||offset>INT64_MAX||fseeko(s->file,(off_t)offset,SEEK_SET))return -1;
 return fread(dst,1,bytes,s->file)==bytes?0:-1;
}
static int source(const char *name,input_file *s,npu_storage_source *out) {
 s->file=fopen(name,"rb");if(!s->file||fseeko(s->file,0,SEEK_END))return -1;
 off_t size=ftello(s->file);if(size<=0)return -1;s->size=(uint64_t)size;
 *out=(npu_storage_source){read_at,s,s->size};return 0;
}
static uint64_t now_ns(void *unused) {
 struct timespec t;(void)unused;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(2);
 return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static unsigned digit(char c){return c>='0'&&c<='9'?(unsigned)(c-'0'):(unsigned)(c-'a'+10);}
static void digest(const char *hex,unsigned char out[32]){for(unsigned i=0;i<32;i++)out[i]=(unsigned char)(digit(hex[2*i])*16+digit(hex[2*i+1]));}
static int save(const char *dir,const char *name,const void *data,size_t size) {
 if(getenv("QW_NO_FILE_OUTPUT"))return 0;
 char path[2048];if(snprintf(path,sizeof path,"%s/%s",dir,name)>=(int)sizeof path)return -1;
 FILE *f=fopen(path,"wb");if(!f)return -1;
 int rc=fwrite(data,1,size,f)==size?0:-1;if(fclose(f))rc=-1;return rc;
}
static FILE *job_census;
static int execute(void *user,const npu_tensor_job *job,npu_tensor_stats *stats) {
 if(job_census)fprintf(job_census,"%u,%u,%u,%u,%u,%u\n",job->opcode,job->n,job->m,job->k,job->param0,job->param1);
 int rc=npu_tensor_execute(user,job,stats);
 if(rc)fprintf(stderr,"TENSOR_EXECUTOR_ERROR rc=%d opcode=%u n=%u a=%08x out=%08x state=%08x\n",rc,job->opcode,job->n,job->a,job->out,job->state);
 return rc==NPU_TENSOR_DRIVER_UNKNOWN?-106:rc?-105:0;
}
typedef struct {uint32_t layer;char stage[40];size_t count;float data[6144];} trace_record;
static trace_record trace_records[320];static unsigned trace_count;static int trace_candidate;
static void compare_trace(void *unused,uint32_t pos,uint32_t layer,const char *stage,const float *data,size_t count){
 (void)unused;if(pos||count>6144)return;
 if(!trace_candidate){if(trace_count>=320)return;trace_record*r=trace_records+trace_count++;
  r->layer=layer;r->count=count;snprintf(r->stage,sizeof r->stage,"%s",stage);memcpy(r->data,data,count*sizeof(float));
 }else for(unsigned t=0;t<trace_count;t++){trace_record*r=trace_records+t;
  if(r->layer!=layer||strcmp(r->stage,stage)||r->count!=count)continue;
  double ss=0,ee=0,mx=0;for(size_t i=0;i<count;i++){double v=data[i]-r->data[i];ss+=(double)r->data[i]*r->data[i];ee+=v*v;if(fabs(v)>mx)mx=fabs(v);}
  printf("TRACE_DRIFT layer=%u stage=%s count=%zu relative=%.8g maxabs=%.8g\n",layer,stage,count,ss?sqrt(ee/ss):sqrt(ee),mx);break;
 }
}
int main(int argc,char **argv) {
 if(argc!=6){fprintf(stderr,"usage: tensor_model_host HEADW4.BIN CPU_RAW.BIN TENSOR_HEAD.JSON output_dir decode_count\n");return 2;}
 unsigned steps=(unsigned)strtoul(argv[5],0,10);if(!steps||steps>64)return 2;
 const char *hex[3]={"8098c9e888efdca25cf09588037e862730c0cc9e64aceef82d23a7c3f9453532",
 "168f02aee67afee5b56edb8683c98f5fa97dae5c48dad720984a4c1ff2e5262d",
 "37d8371580043b99166df40358ec1de778d9a6dc8953598e9cebf55a5035ab30"};
 input_file files[3]={{0}};npu_storage_source src[3];unsigned char sha[3][32];
 for(unsigned i=0;i<3;i++){if(source(argv[i+1],files+i,src+i))return 2;digest(hex[i],sha[i]);}
 qw_options options={0};options.context_tokens=QW_CONTEXT_MAX;options.prefill_chunk_tokens=128;
 options.tile_rows=QW_TILE_ROWS_OPTIMIZED;options.arena_ddr_offset=16u<<20;
 options.system_reserved_bytes=4u<<20;options.framebuffer_reserved_bytes=12u<<20;
 options.mode=NPU_RAW_SOFTWARE_REFERENCE;options.budgets=(npu_raw_budgets){100000000u,1000000u};
 options.weight_sha256=sha[0];options.cpu_sha256=sha[1];options.catalog_sha256=sha[2];
 qw_profile profile={0};options.profile=&profile;options.profile_clock=now_ns;
 if(getenv("QW_TRACE_DRIFT"))options.trace=compare_trace;
 qw_memory_plan plan;size_t capacity=NPU_RAW_DDR_LIMIT-options.arena_ddr_offset;
 int rc=qw_plan(&options,src[0].size_bytes,src[1].size_bytes,src[2].size_bytes,capacity,&plan);
 if(rc)return 1;
 size_t arena_bytes=(plan.arena_bytes+31u)&~(size_t)31u;
 size_t scratch_bytes=qw_tensor_workspace_bytes();
 if(arena_bytes>capacity||scratch_bytes>capacity-arena_bytes)return 1;
 size_t total=arena_bytes+scratch_bytes;unsigned char *arena=aligned_alloc(32,total);
 float *baseline=malloc(QW_VOCAB*sizeof *baseline);
 if(!arena||!baseline)return 2;
 qw_model *model=0;uint64_t start=now_ns(0);
 rc=qw_open_w4_head_candidate(&model,arena,arena_bytes,&options,src,src+1,src+2,&plan,QW_W4_HEAD_TENSOR_NAME);
 printf("MODEL_OPEN rc=%d arena=%zu scratch=%zu context=%u load_host_seconds=%.6f\n",rc,plan.arena_bytes,scratch_bytes,QW_CONTEXT_MAX,(double)(now_ns(0)-start)/1e9);fflush(stdout);
 if(rc)return 1;
 const float *logits=0;
 if(getenv("QW_COMPARE_W4_HEAD")&&qw_enable_w4_head(model,QW_W4_HEAD_TENSOR_NAME))return 1;
 start=now_ns(0);rc=qw_decode(model,16,&logits);
 if(rc||!logits)return 1;
 memcpy(baseline,logits,QW_VOCAB*sizeof *baseline);
 if(save(argv[4],"baseline-token16.f32le.bin",baseline,QW_VOCAB*4))return 2;
 printf("BASELINE token16 host_seconds=%.6f scope=%s_not_official_BF16_model\n",(double)(now_ns(0)-start)/1e9,getenv("QW_COMPARE_W4_HEAD")?"W4_body_W4head":"W4_body_BF16head");fflush(stdout);
 if(qw_reset(model)||qw_enable_w4_head(model,QW_W4_HEAD_TENSOR_NAME))return 1;
 npu_tensor_region grants[5]={{options.arena_ddr_offset+(uint32_t)plan.state_offset,
  ((uint32_t)plan.state_bytes+31u)&~31u,3u},
  {options.arena_ddr_offset+(uint32_t)arena_bytes,(uint32_t)scratch_bytes,3u},
  {options.arena_ddr_offset+(uint32_t)plan.activation_offset,(uint32_t)plan.activation_bytes,3u},
  {options.arena_ddr_offset+(uint32_t)plan.partial_offset,(uint32_t)plan.partial_bytes,1u},
  {options.arena_ddr_offset+(uint32_t)plan.weights_offset,(uint32_t)plan.weights_bytes,1u}};
 npu_tensor_config config={0};config.mode=NPU_TENSOR_SOFTWARE_REFERENCE;
 config.regions=grants;config.region_count=5;
 config.mapping=(npu_tensor_mapping){arena,total,options.arena_ddr_offset};
 npu_tensor_context device;
 if(npu_tensor_init(&device,&config))return 1;
 rc=qw_tensor_enable(model,execute,&device,arena+arena_bytes,scratch_bytes,grants[1].offset);
 printf("TENSOR_ENABLE rc=%d quality_approved=0\n",rc);fflush(stdout);if(rc)return 1;
 const char*chat_path=getenv("QW_CHAT_TOKENIZER");
 if(chat_path){
  input_file token_file={0};npu_storage_source token_src;unsigned char token_sha[32];
  if(source(chat_path,&token_file,&token_src))return 2;
  unsigned char*token_data=malloc((size_t)token_file.size);if(!token_data||read_at(&token_file,0,token_data,(size_t)token_file.size))return 2;
  digest("66df043b96ef2ccaa8d6e24148b0dc03eb022e8f461a88fdc527c178cbe4aa45",token_sha);
  qw_tokenizer tokenizer;if(qt_open(&tokenizer,token_data,(size_t)token_file.size,token_sha))return 2;
  const char*prompt="<|im_start|>user\n1+1=?<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
  uint32_t tokens[128];size_t count=0,workspace_bytes=qt_workspace_bytes(strlen(prompt));void*workspace=malloc(workspace_bytes);
  if(!workspace||qt_encode(&tokenizer,prompt,strlen(prompt),1,tokens,128,&count,workspace,workspace_bytes)||!count)return 2;
  printf("CHAT_SMOKE prompt_tokens=%zu quality_approved=0 timing=HOST_SOFTWARE_REFERENCE\n",count);fflush(stdout);
  size_t processed=0;if(qw_prefill(model,tokens,count-1,&processed)||processed!=count-1)return 1;
  uint32_t next=tokens[count-1],chat_seed=1;
  for(unsigned step=0;step<steps;step++){
   if(qw_decode(model,next,&logits)||qw_sample(logits,QW_VOCAB,0,1,&chat_seed,&next))return 1;
   const char*piece=0;size_t bytes=0;unsigned flags=0;if(qt_piece(&tokenizer,next,&piece,&bytes,&flags))return 1;
   printf("CHAT_TOKEN step=%u token=%u flags=%u piece=[",step,next,flags);fwrite(piece,1,bytes,stdout);puts("]");fflush(stdout);
   if(flags==1)break;
  }
  if(qw_reset(model)||qw_tensor_disable(model)||qw_close(model))return 1;
  free(workspace);free(token_data);fclose(token_file.file);free(baseline);free(arena);
  puts("PASS_ACTUAL_CHAT_SMOKE_EXECUTION quality_approved=0 board_verified=0");return 0;
 }
 char census_path[2048];if(snprintf(census_path,sizeof census_path,"%s/tensor_jobs.csv",argv[4])>=(int)sizeof census_path)return 2;
 if(!getenv("QW_NO_FILE_OUTPUT")){job_census=fopen(census_path,"wb");if(!job_census)return 2;fprintf(job_census,"opcode,n,m,k,param0,param1\n");}
 trace_candidate=1;qw_profile_clear(&profile);uint32_t token=16,seed=1;double total_time=0,relative=0,maxabs=0;
 uint32_t baseline_top=0,candidate_top=0;qw_tensor_status status={0};
 char token_records[8192];size_t record_bytes=0;
 for(unsigned step=0;step<steps;step++) {
  start=now_ns(0);rc=qw_decode(model,token,&logits);double elapsed=(double)(now_ns(0)-start)/1e9;total_time+=elapsed;
  if(qw_tensor_get_status(model,&status))return 1;
  if(rc||!logits){printf("TOKEN_FAILED step=%u rc=%d opcode=%u numeric=0x%x\n",step,rc,status.last_opcode,status.numeric.status);fflush(stdout);return 1;}
  if(!step) {
   double squared=0,errors=0;
   for(uint32_t i=0;i<QW_VOCAB;i++) {
    if(!isfinite(logits[i]))return 1;
    double diff=(double)logits[i]-baseline[i];errors+=diff*diff;squared+=(double)baseline[i]*baseline[i];
    if(fabs(diff)>maxabs)maxabs=fabs(diff);
    if(baseline[i]>baseline[baseline_top])baseline_top=i;
    if(logits[i]>logits[candidate_top])candidate_top=i;
   }
   relative=squared?sqrt(errors/squared):sqrt(errors);
   if(save(argv[4],"candidate-token16.f32le.bin",logits,QW_VOCAB*4))return 2;
    const char *expected_path=getenv("QW_EXPECT_RTL_LOGITS");
    if(expected_path){
     FILE *expected=fopen(expected_path,"rb");if(!expected)return 2;
     uint32_t expected_word,actual_word;
     for(uint32_t q=0;q<QW_VOCAB;q++){
      if(fread(&expected_word,4,1,expected)!=1){fclose(expected);return 2;}
      memcpy(&actual_word,logits+q,4);
      if(actual_word!=expected_word){fprintf(stderr,"RTL_LOGIT_MISMATCH index=%u expected=%08x actual=%08x\n",q,expected_word,actual_word);fclose(expected);return 1;}
     }
     if(fgetc(expected)!=EOF||fclose(expected))return 2;
     printf("FULL_LOGITS_BYTE_EQUAL count=%u\n",QW_VOCAB);fflush(stdout);
    }
   printf("OBSERVED_ERROR relative_l2=%.9g maxabs=%.9g baseline_argmax=%u candidate_argmax=%u quality_approved=0\n",relative,maxabs,baseline_top,candidate_top);
  }
  if(qw_sample(logits,QW_VOCAB,0,1,&seed,&token))return 1;
  uint32_t hash=2166136261u;
  for(uint32_t i=0;!getenv("QW_NO_FILE_OUTPUT")&&i<QW_VOCAB;i++) {
   uint32_t bits;memcpy(&bits,logits+i,4);
   for(unsigned b=0;b<4;b++){hash^=bits&255u;hash*=16777619u;bits>>=8;}
  }
  int written=snprintf(token_records+record_bytes,sizeof token_records-record_bytes,
   "%s{\"step\":%u,\"sampled_token\":%u,\"logits_fnv1a32\":%u}",step?",":"",step,token,hash);
  if(written<0||(size_t)written>=sizeof token_records-record_bytes)return 2;
  record_bytes+=(size_t)written;
  printf("ACTUAL_TOKEN step=%u sampled=%u host_seconds=%.6f tensor_jobs=%" PRIu64 " raw_jobs=%" PRIu64 " saturations=%u underflows=%u\n",step,token,elapsed,status.jobs,status.raw_jobs,status.numeric.saturations,status.numeric.underflows);fflush(stdout);
 }
 if(job_census&&fclose(job_census))return 2;job_census=0;
 char report[10000];int n=snprintf(report,sizeof report,
 "{\"execution_passed\":true,\"quality_approved\":false,\"board_verified\":false,\"timing_domain\":\"HOST_SOFTWARE_REFERENCE\",\"numeric_version\":%u,\"tokens\":%u,\"host_total_seconds\":%.9g,\"host_tokens_per_second\":%.9g,\"relative_l2_token16\":%.9g,\"maxabs_token16\":%.9g,\"baseline_argmax\":%u,\"candidate_argmax\":%u,\"tensor_jobs\":%" PRIu64 ",\"raw_jobs\":%" PRIu64 ",\"saturations\":%u,\"underflows\":%u,\"actual_tokens\":[%s]}\n",
 NPU_TENSOR_NUMERIC_VERSION,steps,total_time,steps/total_time,relative,maxabs,baseline_top,candidate_top,status.jobs,status.raw_jobs,status.numeric.saturations,status.numeric.underflows,token_records);
 if(n<0||(size_t)n>=sizeof report||save(argv[4],"model_result.json",report,(size_t)n))return 2;
 if(qw_reset(model)||qw_tensor_disable(model)||qw_close(model))return 1;
 for(unsigned i=0;i<3;i++)fclose(files[i].file);
 free(baseline);free(arena);puts("PASS_ACTUAL_MODEL_TENSOR_EXECUTION quality_approved=0 board_verified=0");return 0;
}
