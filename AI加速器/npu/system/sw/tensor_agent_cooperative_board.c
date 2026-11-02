/* Independent real tensor model + real RV32 Agent tools candidate.
 * Initialization is blocking. After submit each tick performs <=1 loop step.
 * No SD writes, model answer fixtures, timeout-as-DMA-stop, or quality claim. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "qw_runtime.h"
#include "qw_head.h"
#include "qw_tokenizer.h"
#include "qw_sha256.h"
#include "qw_tensor_v2.h"
#include "qw_agent_protocol_backend.h"
#include "tensor_agent_cooperative_board.h"
#include "npu_tensor_driver.h"
#include "npu_fat32.h"
#include "sd_card.h"
#include "npu_opt_deployment.h"
#include "tensor_agent_deployment.h"
#include "agent_tool_profile.h"
#define UART (*(volatile uint32_t *)(uintptr_t)0x10000000u)
#define DDR_STATUS (*(volatile uint32_t *)(uintptr_t)0x10000200u)
#define TIMER_LO (*(volatile uint32_t *)(uintptr_t)0x10000100u)
#define TIMER_HI (*(volatile uint32_t *)(uintptr_t)0x10000104u)
#define CLOCK_HZ 81000000u
#define ARENA_BASE UINT32_C(0x81000000)
#define ARENA_OFF UINT32_C(0x01000000)
#define APP_BASE UINT32_C(0xbe000000)
#define RESOURCE_BASE UINT32_C(0xbf000000)
#define RESOURCE_LIMIT UINT32_C(0xbf100000)
#define SD_POLLS 400000u
/* Literal-only resources: complete original R4 text and unchanged numeric LUTs.
 * Loaded and SHA256-checked before tokenizer/model init can reference them. */
extern const unsigned char __agent_resource_start[],__agent_resource_end[];
extern const unsigned char agent_resource_sha[32]; /* separate non-LTO pin object */
tensor_agent_ui_fn tensor_agent_external_ui_hook __attribute__((weak)) = 0;
void *tensor_agent_external_ui_context __attribute__((weak)) = 0;
typedef struct {
 npu_fat32_fs fs;npu_fat32_file files[5];npu_storage_source source[3];
 qw_model *model;qw_options options;qw_memory_plan plan;qw_profile profile;
 qw_tokenizer tokenizer;npu_tensor_context device;npu_tensor_region grants[5];
 ag_loop loop;qw_agent_protocol_backend bridge;
 uint32_t tokens[QW_CONTEXT_MAX];char prompt[QW_AGENT_PROMPT_MAX];
 char payload[QW_TOOL_TEXT_MAX+1],receipt[AG_LOOP_RESPONSE_MAX+1];
 uint64_t loop_steps,max_step_ticks;unsigned tool_events;
} tensor_agent_application;
_Static_assert(sizeof(tensor_agent_application)<262144u,"bounded app DDR structure");
static void text(const char*s){while(*s)UART=(uint8_t)*s++;}
static void span(const char*s,size_t n){while(n--)UART=(uint8_t)*s++;}
static void dec(uint32_t x){char b[10];unsigned n=0;do{b[n++]=(char)('0'+x%10);x/=10;}while(x);while(n)UART=(uint8_t)b[--n];}
static void integer(int x){if(x<0){text("-");dec(0u-(uint32_t)x);}else dec((uint32_t)x);}
static void hex(uint32_t x){const char*d="0123456789abcdef";for(int i=7;i>=0;--i)UART=(uint8_t)d[(x>>(4*i))&15u];}
static uint64_t ticks(void){uint32_t hi,lo,h2;do{hi=TIMER_HI;lo=TIMER_LO;h2=TIMER_HI;}while(hi!=h2);return(uint64_t)hi<<32|lo;}
static uint64_t clockfn(void*u){(void)u;return ticks();}
static uint64_t now_ms(void*u){(void)u;return ticks()/(CLOCK_HZ/1000u);}
static void metric(const char*name,uint64_t v){text(" ");text(name);text("=0x");hex((uint32_t)(v>>32));hex((uint32_t)v);}
static void stage(const char*name,int rc,uint64_t start){text("STAGE ");text(name);text(" rc=");integer(rc);metric("ticks",ticks()-start);text("\n");}
static void storage_metric(const char*name){
 sd_card_stats s;sd_card_get_stats(&s);text(name);
 metric("read_calls",s.read_calls);metric("blocks_requested",s.blocks_requested);metric("blocks_completed",s.blocks_completed);
 metric("crc_verified",s.crc_verified);metric("crc_hardware",s.crc_hardware);metric("crc_software",s.crc_software);
 metric("cmd17",s.cmd17);metric("cmd18",s.cmd18);metric("cmd12",s.cmd12);metric("cmd18_fallback",s.cmd18_fallback);
 metric("errors",s.errors);metric("crc_mode",s.crc_mode);text("\n");
}
static void preserve(void){text("UNKNOWN preserve_model_task_arena_resources=1 DMA_stop_claim=0\n");for(;;)__asm__ volatile("nop":::"memory");}
static int blocks(void*u,uint32_t lba,uint32_t n,void*out){(void)u;return sd_card_read_blocks(SD_SPI_CSR_BASE,lba,n,out,SD_POLLS);}
/* Same bounded FAT read pattern as agent_tools_board_20261102, no file writes. */
static int load_file(npu_fat32_file*f,void*dst){uint64_t off=0;while(off<f->size_bytes){size_t n=(size_t)(f->size_bytes-off);if(n>65536u)n=65536u;int rc=npu_fat32_read_exact_at(f,off,(uint8_t*)dst+(size_t)off,n);if(rc)return rc;off+=n;}return 0;}
static void*allocate(uint32_t*next,size_t bytes){uint64_t p=((uint64_t)*next+31u)&~UINT64_C(31);if(p>RESOURCE_BASE||bytes>RESOURCE_BASE-p)return 0;*next=(uint32_t)(p+bytes);return(void*)(uintptr_t)p;}
static void event(void*u,ag_loop_event_kind kind,const char*id,ag_code code,const char*s){tensor_agent_application*a=u;if(kind==AG_LOOP_EVENT_TOOL)++a->tool_events;text("AGENT_EVENT kind=");dec(kind);text(" id=");text(id);text(" code=");text(ag_code_name(code));text(" text=[");text(s);text("]\n");}
static void receipt_event(void*u,const char*id,const char*s){(void)u;text("MODEL_REAL_RECEIPT id=");text(id);text(" ");text(s);text("\n");}
static void token_event(void*u,uint32_t pos,uint32_t token,float selected,const char*piece,size_t bytes,int stop){(void)u;(void)selected;text("MODEL_TOKEN position=");dec(pos);text(" token=");dec(token);text(" stop=");dec((uint32_t)stop);text(" piece=[");if(piece)span(piece,bytes);text("]\n");}
static void generated_event(void*u,unsigned turn,const char*s,size_t n,const char*payload,size_t payload_n,uint32_t stop,const char*json){tensor_agent_application*a=u;(void)payload;(void)payload_n;(void)stop;text("MODEL_SUFFIX turn=");dec(turn);text(" [");span(s,n);text("] boundary=");text(qw_agent_boundary_name(a->bridge.boundary_origin));text("\nMODEL_NORMALIZED ");text(json);text("\n");}
int main(void){
 text("TENSOR_AGENT_COOPERATIVE_CANDIDATE protocol=npu_agent_tool_protocol_v1 quality_approved=0 board_verified=0 latency_bound_verified=0 persistence=volatile\n");
 uint64_t begin=ticks();while(!(DDR_STATUS&1u)){if(DDR_STATUS&0x12u||ticks()-begin>(uint64_t)CLOCK_HZ*15u)return 1;}
 volatile npu_tensor_regs*regs=(volatile npu_tensor_regs*)(uintptr_t)NPU_TENSOR_CSR_BASE;
 if(regs->id!=NPU_TENSOR_ID||regs->version!=NPU_TENSOR_NUMERIC_VERSION){text("FAIL_TENSOR_ID\n");return 2;}
 tensor_agent_application*a=(tensor_agent_application*)(uintptr_t)APP_BASE;memset(a,0,sizeof(*a));
 begin=ticks();int rc=sd_card_init(SD_SPI_CSR_BASE,SD_POLLS);stage("SD_INIT",rc,begin);if(rc)return 3;
 rc=npu_fat32_mount(&a->fs,blocks,0,0);if(rc)return 4;
 /* Resources are an extra candidate artifact; absence/hash mismatch is fatal. */
 rc=npu_fat32_open(&a->fs,agent_resource_name,a->files+4);
 size_t resource_bytes=(size_t)(__agent_resource_end-__agent_resource_start);
 if(rc||!resource_bytes||resource_bytes>RESOURCE_LIMIT-RESOURCE_BASE||a->files[4].size_bytes!=resource_bytes)return 5;
 rc=load_file(a->files+4,(void*)(uintptr_t)RESOURCE_BASE);if(rc)return 6;
 qw_sha256 hash;unsigned char actual[32];qw_sha_init(&hash);qw_sha_update(&hash,(void*)(uintptr_t)RESOURCE_BASE,resource_bytes);qw_sha_final(&hash,actual);
 if(memcmp(actual,agent_resource_sha,32)){text("FAIL_APP_RESOURCE_SHA256\n");return 7;}
 for(unsigned i=0;i<3;++i){rc=npu_fat32_open(&a->fs,opt_sd_names[i],a->files+i);if(rc||a->files[i].size_bytes!=opt_file_sizes[i])return 8;a->source[i]=(npu_storage_source){npu_fat32_read_exact_at,a->files+i,a->files[i].size_bytes};}
 rc=npu_fat32_open(&a->fs,agent_token_name,a->files+3);if(rc||a->files[3].size_bytes!=AGENT_TOKEN_BYTES)return 9;
 uint32_t next=APP_BASE+(uint32_t)sizeof(*a);
 void*token_data=allocate(&next,AGENT_TOKEN_BYTES);size_t workbytes=qt_workspace_bytes(QW_AGENT_PROMPT_MAX);void*work=allocate(&next,workbytes);
 if(!token_data||!work)return 10;
 begin=ticks();rc=load_file(a->files+3,token_data);if(!rc)rc=qt_open(&a->tokenizer,token_data,AGENT_TOKEN_BYTES,agent_token_sha);stage("TOKENIZER_OPEN",rc,begin);if(rc)return 11;
 a->options.context_tokens=QW_CONTEXT_MAX;a->options.prefill_chunk_tokens=128;a->options.tile_rows=8192;
 a->options.arena_ddr_offset=ARENA_OFF;a->options.system_reserved_bytes=4u<<20;a->options.framebuffer_reserved_bytes=12u<<20;
 a->options.mode=NPU_RAW_HARDWARE;a->options.device=npu_dma_mmio();a->options.budgets=(npu_raw_budgets){100000000u,10000000u};
 a->options.weight_sha256=opt_weight_sha;a->options.cpu_sha256=opt_cpu_sha;a->options.catalog_sha256=opt_catalog_sha;
 a->options.profile=&a->profile;a->options.profile_clock=clockfn;
 size_t capacity=APP_BASE-ARENA_BASE;rc=qw_plan(&a->options,a->source[0].size_bytes,a->source[1].size_bytes,a->source[2].size_bytes,capacity,&a->plan);if(rc)return 12;
 uint32_t arena_bytes=((uint32_t)a->plan.arena_bytes+31u)&~31u;size_t wsbytes=qw_tensor_workspace_bytes();
 if(arena_bytes>capacity||wsbytes>capacity-arena_bytes)return 13;
 text("MEMORY");metric("arena_bytes",arena_bytes);metric("tensor_workspace",wsbytes);metric("app_ddr_end",next);metric("resources",resource_bytes);text("\n");
 sd_card_reset_stats();
 begin=ticks();rc=qw_open_w4_head_candidate(&a->model,(void*)(uintptr_t)ARENA_BASE,arena_bytes,&a->options,a->source,a->source+1,a->source+2,&a->plan,QW_W4_HEAD_TENSOR_NAME);stage("MODEL_COLD_LOAD",rc,begin);storage_metric("MODEL_COLD_STORAGE");if(rc)goto cleanup;
 rc=qw_enable_w4_head(a->model,QW_W4_HEAD_TENSOR_NAME);if(rc)goto cleanup;
 a->grants[0]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.state_offset,((uint32_t)a->plan.state_bytes+31u)&~31u,3u};
 a->grants[1]=(npu_tensor_region){ARENA_OFF+arena_bytes,(uint32_t)wsbytes,3u};
 a->grants[2]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.activation_offset,(uint32_t)a->plan.activation_bytes,3u};
 a->grants[3]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.partial_offset,(uint32_t)a->plan.partial_bytes,1u};
 a->grants[4]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.weights_offset,(uint32_t)a->plan.weights_bytes,1u};
 rc=npu_tensor_ag_init(&a->device,a->grants,5,1000000000u,10000000u);if(rc)goto cleanup;
 rc=qw_tensor_enable(a->model,npu_tensor_model_execute,&a->device,(void*)(uintptr_t)(ARENA_BASE+arena_bytes),wsbytes,ARENA_OFF+arena_bytes);if(rc)goto cleanup;
 if(!qw_agent_protocol_init(&a->bridge,a->model,&a->tokenizer,&a->loop,qw_agent_profile,sizeof(qw_agent_profile)-1u,7u,work,workbytes,a->tokens,QW_CONTEXT_MAX,a->prompt,sizeof(a->prompt),a->payload,sizeof(a->payload),a->receipt,sizeof(a->receipt),now_ms,0,0)){rc=QW_ARGUMENT;goto cleanup;}
 a->bridge.trace_user=a;a->bridge.on_token=token_event;a->bridge.on_text=generated_event;a->bridge.on_receipt=receipt_event;
 ag_loop_init(&a->loop,qw_agent_protocol_adapter(&a->bridge),event,a,0,0);
 if(!qw_agent_protocol_enable_cooperative(&a->bridge)){rc=QW_ARGUMENT;goto cleanup;}
 text("BEGIN_REAL_AGENT_TASK [" AGENT_TASK_TEXT "] original_system_bytes=");dec(sizeof(qw_agent_profile)-1u);text("\n");
 sd_card_reset_stats();
 if(ag_loop_submit(&a->loop,AGENT_TASK_TEXT,now_ms(0),AGENT_TIMEOUT_MS,8u)!=AG_LOOP_RUNNING){rc=QW_ARGUMENT;goto cleanup;}
 while(a->loop.status==AG_LOOP_RUNNING){
  uint64_t now=now_ms(0);
  if(tensor_agent_external_ui_hook&&tensor_agent_external_ui_hook(tensor_agent_external_ui_context,&a->loop,now))ag_loop_cancel(&a->loop);
  if(a->loop.status!=AG_LOOP_RUNNING)break;
  now=now_ms(0); /* UI time must not become a stale tool execution timestamp. */
  if(ag_tick(&a->loop.runtime,now)!=AG_OK){ag_loop_cancel(&a->loop);rc=QW_IO;break;}
  /* Exactly one step per tick. No qw_agent_protocol_run blocking wrapper. */
  begin=ticks();(void)ag_loop_step(&a->loop,now);uint64_t elapsed=ticks()-begin;
  ++a->loop_steps;if(elapsed>a->max_step_ticks)a->max_step_ticks=elapsed;
 }
 text("AGENT_STATUS=");text(ag_loop_status_name(a->loop.status));text(" stage=");text(qw_agent_stage_name(a->bridge.stage));text(" model_error=");integer(a->bridge.model_error);metric("loop_steps",a->loop_steps);metric("observed_max_step_ticks",a->max_step_ticks);text(" quality_approved=0\n");
 if(a->bridge.stage==QW_AGENT_STAGE_UNKNOWN_LEASE||a->bridge.model_error==QW_UNKNOWN)preserve();
 storage_metric("AGENT_WARM_STORAGE");
 {sd_card_stats s;sd_card_get_stats(&s);if(s.read_calls){text("FAIL_WARM_AGENT_SD_READ\n");rc=QW_IO;}}
 if(!rc&&a->loop.status!=AG_LOOP_FINISHED&&a->loop.status!=AG_LOOP_CLARIFY)rc=QW_FORMAT;
cleanup:
 if(rc==QW_UNKNOWN)preserve();
 if(a->model){qw_status status;int sr=qw_get_status(a->model,&status);if(sr==QW_UNKNOWN||(!sr&&(status.lease_active||status.last_error==QW_UNKNOWN)))preserve();int close=qw_close(a->model);if(close==QW_UNKNOWN)preserve();if(!rc)rc=close;}
 text("CANDIDATE_EXIT rc=");integer(rc);text(" quality_approved=0 task_acceptance_not_asserted=1\n");return rc?20:0;
}
