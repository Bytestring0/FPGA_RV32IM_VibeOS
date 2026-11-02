/* Real RV32 hardware model entry. Actual model outputs only; optional golden
 * logits hashes detect execution mismatches, never fill or select outputs.
 * No SD writes, Flash, scripted answers or timeout-as-cancel recovery. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "qw_runtime.h"
#include "qw_head.h"
#include "qw_tensor_v2.h"
#include "npu_tensor_driver.h"
#include "npu_fat32.h"
#include "sd_card.h"
#include "npu_opt_deployment.h"
#include "tensor_expected.h"
#define UART (*(volatile uint32_t *)(uintptr_t)0x10000000u)
#define DDR_STATUS (*(volatile uint32_t *)(uintptr_t)0x10000200u)
#define TIMER_LO (*(volatile uint32_t *)(uintptr_t)0x10000100u)
#define TIMER_HI (*(volatile uint32_t *)(uintptr_t)0x10000104u)
#define CLOCK_HZ 81000000u
#define APP_BASE UINT32_C(0xbfff8000)
#define ARENA_BASE UINT32_C(0x81000000)
#define ARENA_OFF UINT32_C(0x01000000)
typedef struct {
 npu_fat32_fs fs;npu_fat32_file files[3];npu_storage_source source[3];
 qw_model *model;qw_options options;qw_memory_plan plan;qw_profile profile;
 npu_tensor_context device;npu_tensor_region grants[5];qw_tensor_status tensor;
 const float *logits;uint32_t seed;
} tensor_application;
_Static_assert(sizeof(tensor_application)<=32768u,"DDR application reserve");
static void text(const char *s){while(*s)UART=(uint8_t)*s++;}
static void dec(uint32_t x){char a[10];unsigned n=0;do{a[n++]=(char)('0'+x%10);x/=10;}while(x);while(n)UART=(uint8_t)a[--n];}
static void hex(uint32_t x){const char *d="0123456789abcdef";for(int i=7;i>=0;i--)UART=(uint8_t)d[(x>>(4*i))&15u];}
static uint64_t ticks(void){uint32_t hi,lo,h2;do{hi=TIMER_HI;lo=TIMER_LO;h2=TIMER_HI;}while(hi!=h2);return (uint64_t)hi<<32|lo;}
static uint64_t clockfn(void *u){(void)u;return ticks();}
static void metric(const char *name,uint64_t value){text(" ");text(name);text("=0x");hex((uint32_t)(value>>32));hex((uint32_t)value);}
static void stage(const char *name,int rc,uint64_t start){text("STAGE ");text(name);text(" rc=");if(rc<0){text("-");dec(0u-(uint32_t)rc);}else dec((uint32_t)rc);metric("ticks",ticks()-start);text("\n");}
static void storage_metric(const char *name){
 sd_card_stats s;sd_card_get_stats(&s);text(name);
 metric("read_calls",s.read_calls);metric("blocks_requested",s.blocks_requested);
 metric("blocks_completed",s.blocks_completed);metric("crc_verified",s.crc_verified);
 metric("crc_hardware",s.crc_hardware);metric("crc_software",s.crc_software);
 metric("cmd17",s.cmd17);metric("cmd18",s.cmd18);metric("cmd12",s.cmd12);
 metric("cmd18_fallback",s.cmd18_fallback);metric("errors",s.errors);metric("crc_mode",s.crc_mode);text("\n");
}
static void preserve(void){text("UNKNOWN preserve_model_arena_workspace_state=1\n");for(;;)__asm__ volatile("nop":::"memory");}
static int blocks(void *user,uint32_t lba,uint32_t count,void *out){(void)user;return sd_card_read_blocks(SD_SPI_CSR_BASE,lba,count,out,400000u);}
static uint32_t logits_hash(const float *values){uint32_t h=2166136261u;for(uint32_t i=0;i<QW_VOCAB;i++){uint32_t bits;memcpy(&bits,values+i,4);for(unsigned b=0;b<4;b++){h^=bits&255u;h*=16777619u;bits>>=8;}}return h;}
int main(void) {
 text("TENSOR_V2_REAL_MODEL_BOARD graph=integer_Q16_Q24 numerical_version=0x00020000 quality_approved=0 cpu_hz=81000000\n");
 uint64_t begin=ticks();while(!(DDR_STATUS&1u)){if(DDR_STATUS&0x12u||ticks()-begin>(uint64_t)CLOCK_HZ*15u)return 1;}
 stage("DDR_READY",0,begin);
 volatile npu_tensor_regs *regs=(volatile npu_tensor_regs *)(uintptr_t)NPU_TENSOR_CSR_BASE;
 if(regs->id!=NPU_TENSOR_ID||regs->version!=NPU_TENSOR_NUMERIC_VERSION){text("FAIL_TENSOR_ID\n");return 2;}
 tensor_application *app=(tensor_application *)(uintptr_t)APP_BASE;memset(app,0,sizeof *app);
 begin=ticks();int rc=sd_card_init(SD_SPI_CSR_BASE,400000u);stage("SD_INIT",rc,begin);if(rc)return 3;
 begin=ticks();rc=npu_fat32_mount(&app->fs,blocks,0,0);stage("FAT32_MOUNT",rc,begin);if(rc)return 4;
 for(unsigned i=0;i<3;i++) {
  rc=npu_fat32_open(&app->fs,opt_sd_names[i],app->files+i);
  if(rc||app->files[i].size_bytes!=opt_file_sizes[i]){text("FAIL_FILE\n");return 5;}
  app->source[i]=(npu_storage_source){npu_fat32_read_exact_at,app->files+i,app->files[i].size_bytes};
 }
 app->options.context_tokens=2048;app->options.prefill_chunk_tokens=128;
 app->options.tile_rows=8192;app->options.arena_ddr_offset=ARENA_OFF;
 app->options.system_reserved_bytes=4u<<20;app->options.framebuffer_reserved_bytes=12u<<20;
 app->options.mode=NPU_RAW_HARDWARE;app->options.device=npu_dma_mmio();
 app->options.budgets=(npu_raw_budgets){100000000u,10000000u};
 app->options.weight_sha256=opt_weight_sha;app->options.cpu_sha256=opt_cpu_sha;app->options.catalog_sha256=opt_catalog_sha;
 app->options.profile=&app->profile;app->options.profile_clock=clockfn;
 size_t capacity=APP_BASE-ARENA_BASE;
 rc=qw_plan(&app->options,app->source[0].size_bytes,app->source[1].size_bytes,app->source[2].size_bytes,capacity,&app->plan);
 if(rc)return 7;
 uint32_t arena_bytes=((uint32_t)app->plan.arena_bytes+31u)&~31u;
 size_t wsbytes=qw_tensor_workspace_bytes();
 if(arena_bytes>capacity||wsbytes>capacity-arena_bytes)return 8;
 text("MEMORY");metric("arena_bytes",arena_bytes);metric("tensor_workspace",wsbytes);text("\n");
 sd_card_reset_stats();
  begin=ticks();rc=qw_open_w4_head_candidate(&app->model,(void *)(uintptr_t)ARENA_BASE,arena_bytes,&app->options,app->source,app->source+1,app->source+2,&app->plan,QW_W4_HEAD_TENSOR_NAME);
 stage("MODEL_COLD_LOAD",rc,begin);storage_metric("MODEL_COLD_STORAGE");if(rc==QW_UNKNOWN)preserve();if(rc)return 9;
 if(qw_enable_w4_head(app->model,QW_W4_HEAD_TENSOR_NAME))return 10;
 app->grants[0]=(npu_tensor_region){ARENA_OFF+(uint32_t)app->plan.state_offset,((uint32_t)app->plan.state_bytes+31u)&~31u,3u};
 app->grants[1]=(npu_tensor_region){ARENA_OFF+arena_bytes,(uint32_t)wsbytes,3u};
 app->grants[2]=(npu_tensor_region){ARENA_OFF+(uint32_t)app->plan.activation_offset,(uint32_t)app->plan.activation_bytes,3u};
 app->grants[3]=(npu_tensor_region){ARENA_OFF+(uint32_t)app->plan.partial_offset,(uint32_t)app->plan.partial_bytes,1u};
 app->grants[4]=(npu_tensor_region){ARENA_OFF+(uint32_t)app->plan.weights_offset,(uint32_t)app->plan.weights_bytes,1u};
 if(npu_tensor_ag_init(&app->device,app->grants,5,1000000000u,10000000u))return 11;
 rc=qw_tensor_enable(app->model,npu_tensor_model_execute,&app->device,(void *)(uintptr_t)(ARENA_BASE+arena_bytes),wsbytes,ARENA_OFF+arena_bytes);
 if(rc)return 12;
 sd_card_reset_stats();
  app->seed=1;uint32_t token=16;uint64_t total_start=ticks();
 for(unsigned step=0;step<TENSOR_EXPECTED_COUNT;step++) {
  begin=ticks();rc=qw_decode(app->model,token,&app->logits);uint64_t duration=ticks()-begin;
  stage("ACTUAL_DECODE",rc,begin);if(rc==QW_UNKNOWN)preserve();if(rc)return 13;
  if(qw_tensor_get_status(app->model,&app->tensor))return 14;
  if(qw_sample(app->logits,QW_VOCAB,0,1,&app->seed,&token))return 15;
  uint32_t hash=logits_hash(app->logits);
  text("ACTUAL_TOKEN step=");dec(step);text(" sampled=");dec(token);text(" logits_fnv1a32=0x");hex(hash);
  metric("decode_ticks",duration);metric("tensor_jobs",app->tensor.jobs);metric("matrix_jobs",app->tensor.raw_jobs);metric("saturations",app->tensor.numeric.saturations);text("\n");
  if(hash!=tensor_expected_hash[step]||token!=tensor_expected_token[step]){text("FAIL_ACTUAL_MODEL_LOGITS\n");return 16;}
 }
 text("MODEL_THROUGHPUT");metric("tokens",TENSOR_EXPECTED_COUNT);metric("total_ticks",ticks()-total_start);metric("cpu_hz",CLOCK_HZ);text("\n");
 storage_metric("MODEL_WARM_STORAGE");
  {sd_card_stats s;sd_card_get_stats(&s);if(s.read_calls){text("FAIL_WARM_MODEL_SD_READ\n");return 18;}}
  rc=qw_close(app->model);if(rc==QW_UNKNOWN)preserve();if(rc)return 17;
 text("PASS_TENSOR_REAL_MODEL execution_hash_match=1 quality_approved=0 no_sd_or_flash_writes=1\n");return 0;
}
