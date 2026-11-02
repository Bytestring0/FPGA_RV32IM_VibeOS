/* Actual immutable full-model comparison, not a synthetic operator demo.
 * Timing is explicitly HOST SOFTWARE_REFERENCE, never FPGA throughput. */
#define _POSIX_C_SOURCE 200809L
#include "qw_runtime.h"
#include "qw_head.h"
#include "qw_tensor_v2.h"
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
 char path[2048];if(snprintf(path,sizeof path,"%s/%s",dir,name)>=(int)sizeof path)return -1;
 FILE *f=fopen(path,"wb");if(!f)return -1;
 int rc=fwrite(data,1,size,f)==size?0:-1;if(fclose(f))rc=-1;return rc;
}
static int execute(void *user,const npu_tensor_job *job,npu_tensor_stats *stats) {
 int rc=npu_tensor_execute(user,job,stats);
 if(rc)fprintf(stderr,"TENSOR_EXECUTOR_ERROR rc=%d opcode=%u n=%u a=%08x out=%08x state=%08x\n",rc,job->opcode,job->n,job->a,job->out,job->state);
 return rc==NPU_TENSOR_DRIVER_UNKNOWN?-106:rc?-105:0;
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
 const float *logits=0;start=now_ns(0);rc=qw_decode(model,16,&logits);
 if(rc||!logits)return 1;
 memcpy(baseline,logits,QW_VOCAB*sizeof *baseline);
 if(save(argv[4],"baseline-token16.f32le.bin",baseline,QW_VOCAB*4))return 2;
 printf("BASELINE token16 host_seconds=%.6f scope=W4_body_BF16head_not_official_BF16_model\n",(double)(now_ns(0)-start)/1e9);fflush(stdout);
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
 qw_profile_clear(&profile);uint32_t token=16,seed=1;double total_time=0,relative=0,maxabs=0;
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
   printf("OBSERVED_ERROR relative_l2=%.9g maxabs=%.9g baseline_argmax=%u candidate_argmax=%u quality_approved=0\n",relative,maxabs,baseline_top,candidate_top);
  }
  if(qw_sample(logits,QW_VOCAB,0,1,&seed,&token))return 1;
  uint32_t hash=2166136261u;
  for(uint32_t i=0;i<QW_VOCAB;i++) {
   uint32_t bits;memcpy(&bits,logits+i,4);
   for(unsigned b=0;b<4;b++){hash^=bits&255u;hash*=16777619u;bits>>=8;}
  }
  int written=snprintf(token_records+record_bytes,sizeof token_records-record_bytes,
   "%s{\"step\":%u,\"sampled_token\":%u,\"logits_fnv1a32\":%u}",step?",":"",step,token,hash);
  if(written<0||(size_t)written>=sizeof token_records-record_bytes)return 2;
  record_bytes+=(size_t)written;
  printf("ACTUAL_TOKEN step=%u sampled=%u host_seconds=%.6f tensor_jobs=%" PRIu64 " raw_jobs=%" PRIu64 " saturations=%u underflows=%u\n",step,token,elapsed,status.jobs,status.raw_jobs,status.numeric.saturations,status.numeric.underflows);fflush(stdout);
 }
 char report[10000];int n=snprintf(report,sizeof report,
 "{\"execution_passed\":true,\"quality_approved\":false,\"board_verified\":false,\"timing_domain\":\"HOST_SOFTWARE_REFERENCE\",\"numeric_version\":%u,\"tokens\":%u,\"host_total_seconds\":%.9g,\"host_tokens_per_second\":%.9g,\"relative_l2_token16\":%.9g,\"maxabs_token16\":%.9g,\"baseline_argmax\":%u,\"candidate_argmax\":%u,\"tensor_jobs\":%" PRIu64 ",\"raw_jobs\":%" PRIu64 ",\"saturations\":%u,\"underflows\":%u,\"actual_tokens\":[%s]}\n",
 NPU_TENSOR_NUMERIC_VERSION,steps,total_time,steps/total_time,relative,maxabs,baseline_top,candidate_top,status.jobs,status.raw_jobs,status.numeric.saturations,status.numeric.underflows,token_records);
 if(n<0||(size_t)n>=sizeof report||save(argv[4],"model_result.json",report,(size_t)n))return 2;
 if(qw_reset(model)||qw_tensor_disable(model)||qw_close(model))return 1;
 for(unsigned i=0;i<3;i++)fclose(files[i].file);
 free(baseline);free(arena);puts("PASS_ACTUAL_MODEL_TENSOR_EXECUTION quality_approved=0 board_verified=0");return 0;
}
