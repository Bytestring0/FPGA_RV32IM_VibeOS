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
#include "npu_runtime.h"
#include "npu_fat32.h"
#include "sd_card.h"
#include "npu_opt_deployment.h"
#include "tensor_agent_deployment.h"
#include "agent_tool_profile.h"
#ifdef AGENT_UART_ASSETS
#include "ag_asset_loader.h"
#endif
#ifdef AGENT_INTERACTIVE_UI
#include "ag_agent_ui.h"
#include "ag_coremark_ui.h"
#endif
#define UART (*(volatile uint32_t *)(uintptr_t)0x10000000u)
#define DDR_STATUS (*(volatile uint32_t *)(uintptr_t)0x10000200u)
#define TIMER_LO (*(volatile uint32_t *)(uintptr_t)0x10000100u)
#define TIMER_HI (*(volatile uint32_t *)(uintptr_t)0x10000104u)
#define CLOCK_HZ AGENT_CLOCK_HZ
#ifndef AGENT_MODEL_SMOKE_TOKENS
#define AGENT_MODEL_SMOKE_TOKENS 0
#endif
#define ARENA_BASE UINT32_C(0x81000000)
#define ARENA_OFF UINT32_C(0x01000000)
#define APP_BASE UINT32_C(0xbe000000)
#define RESOURCE_BASE UINT32_C(0xbf000000)
#define RESOURCE_LIMIT UINT32_C(0xbf100000)
#define SD_POLLS 400000u
/* Literal-only resources: complete original R4 text and unchanged numeric LUTs.
 * Loaded and SHA256-checked before tokenizer/model init can reference them. */
extern const unsigned char __agent_resource_start[],__agent_resource_end[];
extern unsigned char __ddr_bss_start[],__ddr_bss_end[];
extern const unsigned char agent_resource_sha[32]; /* separate non-LTO pin object */
tensor_agent_ui_fn tensor_agent_external_ui_hook __attribute__((weak)) = 0;
void *tensor_agent_external_ui_context __attribute__((weak)) = 0;
typedef struct {
 npu_fat32_fs fs;npu_fat32_file files[6];npu_storage_source source[3];
 qw_model *model;qw_options options;qw_memory_plan plan;qw_profile profile;
 qw_tokenizer tokenizer;npu_tensor_context device;npu_tensor_region grants[5];
 ag_loop loop;qw_agent_protocol_backend bridge;
#ifdef AGENT_INTERACTIVE_UI
 ag_agent_ui ui;ag_runtime saved_tools;const unsigned char *font;unsigned ui_ready,task_key;
#endif
 uint32_t tokens[QW_CONTEXT_MAX];char prompt[QW_AGENT_PROMPT_MAX];
 char payload[QW_TOOL_TEXT_MAX+1],receipt[AG_LOOP_RESPONSE_MAX+1];
 uint64_t loop_steps,max_step_ticks,task_start_ticks,first_token_ticks,last_token_ticks;
 uint64_t sampled_tokens,generated_tokens,generated_bytes,decode_intervals,decode_interval_ticks;
 uint64_t turn_last_token_ticks;unsigned turn_seen,tool_events;
#ifdef AGENT_PROFILE_DEVICE
 uint64_t tensor_driver_ticks[23],tensor_device_cycles[23];uint32_t tensor_calls[23];
#endif
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
#ifdef AGENT_PROFILE_DEVICE
static int profiled_tensor(void*u,const npu_tensor_job*j,npu_tensor_stats*s){
 tensor_agent_application*a=u;uint64_t start=ticks();int rc=npu_tensor_model_execute(&a->device,j,s);
 if(j->opcode<23){a->tensor_driver_ticks[j->opcode]+=ticks()-start;a->tensor_device_cycles[j->opcode]+=a->device.last_cycles;a->tensor_calls[j->opcode]++;}
 return rc;
}
#endif
#ifdef AGENT_UART_ASSETS
static int asset_rx(void*u,unsigned char*out,uint32_t timeout_ms){
 (void)u;volatile uint32_t*r=(volatile uint32_t*)(uintptr_t)0x10001240u;
 uint64_t start=ticks();for(;;){uint32_t s=r[1];if(s&6u)return -1;if(s&1u){*out=(unsigned char)r[0];return 0;}if(ticks()-start>(uint64_t)(CLOCK_HZ/1000u)*timeout_ms)return -1;}
}
static int asset_tx(void*u,const unsigned char*p,size_t n){(void)u;while(n--)UART=*p++;return 0;}
static int asset_load(const char*name,void*destination,uint32_t bytes){
 text("ASSET_READY ");text(name);text(" bytes=");dec(bytes);text("\n");
 ag_asset_transport transport={0,asset_rx,asset_tx};int rc=ag_asset_receive(&transport,destination,bytes);
 text("ASSET_DONE ");text(name);text(" rc=");integer(rc);text("\n");return rc;
}
#endif
#ifdef AGENT_INTERACTIVE_UI
static uint32_t ui_read(void*u,uint32_t address){(void)u;uint32_t v=*(volatile uint32_t*)(uintptr_t)address;if(address==0x10001210u&&(v&0x7fffu)){text("UI_KEY events=0x");hex(v);text("\n");}return v;}
static void ui_write(void*u,uint32_t address,uint32_t value){(void)u;*(volatile uint32_t*)(uintptr_t)address=value;}
static int ui_glyph(void*u,uint32_t cp,unsigned char rows[16]){
 tensor_agent_application*a=u;if(cp>127)cp='?';memcpy(rows,a->font+cp*16,16);return 0;
}
#endif
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
static ag_agent_ui*load_ui;static uint64_t load_done,load_total,load_shown;
static void load_add(uint64_t n){load_done+=n;
#ifdef AGENT_INTERACTIVE_UI
 if(!load_ui||!load_total)return;
 if(load_done-load_shown>=load_total/100u||load_done>=load_total){
  load_shown=load_done;ag_agent_ui_progress(load_ui,"Loading model bytes from SD card",(unsigned)(load_done*1000u/load_total));}
#else
 (void)n;
#endif
}
static int progress_read(void*user,uint64_t off,void*dst,size_t n){int rc=npu_fat32_read_exact_at((npu_fat32_file*)user,off,(uint8_t*)dst,n);if(!rc)load_add(n);return rc;}
/* Per-token tied-embedding row fetch; must not count toward load progress. */
static int embedding_row_read(void*user,uint64_t off,void*dst,size_t n){return npu_fat32_read_exact_at((npu_fat32_file*)user,off,(uint8_t*)dst,n);}
static int load_file(npu_fat32_file*f,void*dst){uint64_t off=0;while(off<f->size_bytes){size_t n=(size_t)(f->size_bytes-off);if(n>65536u)n=65536u;int rc=npu_fat32_read_exact_at(f,off,(uint8_t*)dst+(size_t)off,n);if(rc)return rc;off+=n;load_add(n);}return 0;}
static void*allocate(uint32_t*next,size_t bytes){uint64_t p=((uint64_t)*next+31u)&~UINT64_C(31);if(p>RESOURCE_BASE||bytes>RESOURCE_BASE-p)return 0;*next=(uint32_t)(p+bytes);return(void*)(uintptr_t)p;}
static void event(void*u,ag_loop_event_kind kind,const char*id,ag_code code,const char*s){tensor_agent_application*a=u;if(kind==AG_LOOP_EVENT_TOOL)++a->tool_events;text("AGENT_EVENT kind=");dec(kind);text(" id=");text(id);text(" code=");text(ag_code_name(code));text(" text=[");text(s);text("]\n");}
static void receipt_event(void*u,const char*id,const char*s){
#ifdef AGENT_INTERACTIVE_UI
 tensor_agent_application*a=u;if(a->ui_ready)ag_agent_ui_receipt(&a->ui,s);
#else
 (void)u;
#endif
text("MODEL_REAL_RECEIPT id=");text(id);text(" ");text(s);text("\n");}
/* Count only model samples. Provided structural and chat boundary tokens never enter here. */
static void token_event(void*u,uint32_t pos,uint32_t token,float selected,const char*piece,size_t bytes,int stop){
 tensor_agent_application*a=u;uint64_t now=ticks();(void)selected;++a->sampled_tokens;
 if(!stop){
#ifdef AGENT_INTERACTIVE_UI
   if(a->ui_ready&&piece){
    ag_agent_ui_piece(&a->ui,piece,bytes);
    /* Direct chat has no agent loop, so drive the display from here. */
    (void)ag_agent_ui_poll(&a->ui,0,now_ms(0));
   }
#endif
  if(!a->generated_tokens)a->first_token_ticks=now;
  if(a->turn_seen==a->bridge.turn+1u){++a->decode_intervals;a->decode_interval_ticks+=now-a->turn_last_token_ticks;}
  a->turn_seen=a->bridge.turn+1u;a->turn_last_token_ticks=now;a->last_token_ticks=now;
  ++a->generated_tokens;a->generated_bytes+=bytes;
 }
 if(AGENT_TRACE_TOKENS){text("MODEL_TOKEN position=");dec(pos);text(" token=");dec(token);text(" stop=");dec((uint32_t)stop);text(" piece=[");if(piece)span(piece,bytes);text("]\n");}
}
static void generation_metrics(tensor_agent_application*a,uint64_t end){
 uint64_t elapsed=end-a->task_start_ticks;
 text("AGENT_GENERATION_STATS");metric("clock_hz",CLOCK_HZ);metric("task_ticks",elapsed);
 metric("sampled_tokens",a->sampled_tokens);metric("generated_tokens",a->generated_tokens);metric("generated_bytes",a->generated_bytes);
 metric("ttft_valid",a->generated_tokens!=0);metric("ttft_ticks",a->generated_tokens?a->first_token_ticks-a->task_start_ticks:0);
 metric("decode_intervals",a->decode_intervals);metric("decode_interval_ticks",a->decode_interval_ticks);
 metric("decode_millitokens_per_second",a->decode_interval_ticks?a->decode_intervals*(uint64_t)CLOCK_HZ*1000u/a->decode_interval_ticks:0);
 metric("task_millitokens_per_second",elapsed?a->generated_tokens*(uint64_t)CLOCK_HZ*1000u/elapsed:0);
 metric("model_tokens_completed",a->profile.tokens_completed);metric("model_tokens_failed",a->profile.tokens_failed);
 metric("trace_tokens",AGENT_TRACE_TOKENS);text(" provided_tokens_counted=0 throughput_target_verified=0\n");
}
static void generated_event(void*u,unsigned turn,const char*s,size_t n,const char*payload,size_t payload_n,uint32_t stop,const char*json){tensor_agent_application*a=u;(void)payload;(void)payload_n;(void)stop;text("MODEL_SUFFIX turn=");dec(turn);text(" [");span(s,n);text("] boundary=");text(qw_agent_boundary_name(a->bridge.boundary_origin));text("\nMODEL_NORMALIZED ");text(json);text("\n");}
void agent_trap_report(uint32_t cause,uint32_t pc,uint32_t value){text("CPU_TRAP cause=0x");hex(cause);text(" pc=0x");hex(pc);text(" value=0x");hex(value);text("\n");}
int main(void){
 text("TENSOR_AGENT_COOPERATIVE_CANDIDATE protocol=npu_agent_tool_protocol_v1 quality_approved=0 board_verified=0 latency_bound_verified=0 persistence=volatile\n");
 uint64_t begin=ticks();while(!(DDR_STATUS&1u)){if(DDR_STATUS&0x12u||ticks()-begin>(uint64_t)CLOCK_HZ*15u)return 1;}
 volatile npu_tensor_regs*regs=(volatile npu_tensor_regs*)(uintptr_t)NPU_TENSOR_CSR_BASE;
 if(regs->id!=NPU_TENSOR_ID||regs->version!=NPU_TENSOR_NUMERIC_VERSION){text("FAIL_TENSOR_ID\n");return 2;}
 tensor_agent_application*a=(tensor_agent_application*)(uintptr_t)APP_BASE;memset(a,0,sizeof(*a));
 begin=ticks();int rc=sd_card_init(SD_SPI_CSR_BASE,SD_POLLS);stage("SD_INIT",rc,begin);if(rc)return 3;
 rc=npu_fat32_mount(&a->fs,blocks,0,0);if(rc)return 4;
 /* Resource length and linker destination are checked before publication. */
 size_t resource_bytes=(size_t)(__agent_resource_end-__agent_resource_start);
 if(!resource_bytes||resource_bytes>RESOURCE_LIMIT-RESOURCE_BASE)return 5;
#ifdef AGENT_UART_ASSETS
 if(asset_load("APPRES.BIN",(void*)(uintptr_t)__agent_resource_start,(uint32_t)resource_bytes))return 6;
#else
 if(AGENT_RESOURCES_ON_SD){
  if((uintptr_t)__agent_resource_start!=RESOURCE_BASE||(uintptr_t)__agent_resource_end>RESOURCE_LIMIT)return 5;
  rc=npu_fat32_open(&a->fs,agent_resource_name,a->files+4);
  if(rc||a->files[4].size_bytes!=resource_bytes)return 5;
  rc=load_file(a->files+4,(void*)(uintptr_t)__agent_resource_start);if(rc)return 6;
 }
#endif
#ifdef AGENT_INTERACTIVE_UI
  if(ag_cache_maintain_range((uint32_t)(uintptr_t)__agent_resource_start-0x80000000u,((uint32_t)resource_bytes+31u)&~31u,10000000u))return 6;
  __asm__ volatile("fence iorw,iorw; fence.i" ::: "memory");
#endif

 for(unsigned i=0;i<3;++i){rc=npu_fat32_open(&a->fs,opt_sd_names[i],a->files+i);if(rc||a->files[i].size_bytes!=opt_file_sizes[i])return 8;a->source[i]=(npu_storage_source){progress_read,a->files+i,a->files[i].size_bytes};}
 rc=npu_fat32_open(&a->fs,agent_token_name,a->files+3);if(rc||a->files[3].size_bytes!=AGENT_TOKEN_BYTES)return 9;
 uint32_t next=APP_BASE+(uint32_t)sizeof(*a);
 void*token_data=allocate(&next,AGENT_TOKEN_BYTES);size_t workbytes=qt_workspace_bytes(QW_AGENT_PROMPT_MAX);void*work=allocate(&next,workbytes);
 if(!token_data||!work)return 10;
#ifdef AGENT_INTERACTIVE_UI
  a->font=allocate(&next,2048u);if(!a->font)return 10;
#ifdef AGENT_UART_ASSETS
  if(asset_load("FONT.BIN",(void*)a->font,2048u))return 10;
#else
  static const char font_name[11]={'F','O','N','T',' ',' ',' ',' ','B','I','N'};
  rc=npu_fat32_open(&a->fs,font_name,a->files+5);if(rc||a->files[5].size_bytes!=2048u)return 10;
  rc=load_file(a->files+5,(void*)a->font);if(rc)return 10;
#endif
  memset(__ddr_bss_start,0,(size_t)(__ddr_bss_end-__ddr_bss_start));
  ag_display_io io={a,ui_read,ui_write,clockfn,0,CLOCK_HZ};
  int ui_rc=ag_agent_ui_init(&a->ui,&io,ui_glyph,a);a->ui_ready=ui_rc==0;
  text("HDMI_UI_INIT rc=");integer(ui_rc);text("\n");if(!a->ui_ready)return 14;
  ag_agent_ui_stage(&a->ui,"Ready: choose an AI task or run CoreMark first.");
  text("UI_BOOT_READY keyboard=screen,UART coremark=live model=not_loaded\n");
  for(;;){int choice=ag_agent_ui_poll(&a->ui,0,now_ms(0));if(choice==2){(void)ag_coremark_ui_run(&a->ui,now_ms(0));text("UI_BOOT_READY coremark=finished model=not_loaded\n");continue;}if(choice==1)break;}
  ag_agent_ui_stage(&a->ui,"Loading full model. Please wait.");
  load_ui=&a->ui;load_done=0;load_shown=0;
  /* load_total is computed after the lazy-embedding decision below, otherwise the
   * bar would saturate early when the full tied matrix is actually streamed. */
#endif
 begin=ticks();rc=load_file(a->files+3,token_data);if(!rc)rc=qt_open(&a->tokenizer,token_data,AGENT_TOKEN_BYTES,agent_token_sha);stage("TOKENIZER_OPEN",rc,begin);if(rc)return 11;
 a->options.context_tokens=QW_CONTEXT_MAX;a->options.prefill_chunk_tokens=128;a->options.tile_rows=8192;
 a->options.arena_ddr_offset=ARENA_OFF;a->options.system_reserved_bytes=4u<<20;a->options.framebuffer_reserved_bytes=12u<<20;
 a->options.mode=NPU_RAW_HARDWARE;a->options.device=npu_dma_mmio();a->options.budgets=(npu_raw_budgets){100000000u,10000000u};
 a->options.weight_sha256=opt_weight_sha;a->options.cpu_sha256=opt_cpu_sha;a->options.catalog_sha256=opt_catalog_sha;
 a->options.profile=&a->profile;a->options.profile_clock=clockfn;
 /* Lazy tied-embedding load saves ~3.5 min of SD streaming but costs ~1.5 s
  * per token because a random 2 KB read re-walks the FAT chain. Off by default
  * so interactive generation keeps the fast path. */
 a->options.lazy_embedding=0;a->options.embedding_user=a->files+1;a->options.embedding_read=embedding_row_read;
#ifdef AGENT_INTERACTIVE_UI
 /* Lazy mode streams only the small head/tail spans of CPU_RAW; full mode streams all of it. */
 load_total=(uint64_t)AGENT_TOKEN_BYTES+(uint64_t)a->source[0].size_bytes+a->source[2].size_bytes
            +((a->options.lazy_embedding&&a->source[1].size_bytes>4194304u)
                ?4194304u:(uint64_t)a->source[1].size_bytes);
#endif
 size_t capacity=APP_BASE-ARENA_BASE;rc=qw_plan(&a->options,a->source[0].size_bytes,a->source[1].size_bytes,a->source[2].size_bytes,capacity,&a->plan);if(rc)return 12;
 uint32_t arena_bytes=((uint32_t)a->plan.arena_bytes+31u)&~31u;size_t wsbytes=qw_tensor_workspace_bytes();
 if(arena_bytes>capacity||wsbytes>capacity-arena_bytes)return 13;
 text("MEMORY");metric("arena_bytes",arena_bytes);metric("tensor_workspace",wsbytes);metric("app_ddr_end",next);metric("resources",resource_bytes);text("\n");
 sd_card_reset_stats();
 begin=ticks();rc=qw_open_w4_head_candidate(&a->model,(void*)(uintptr_t)ARENA_BASE,arena_bytes,&a->options,a->source,a->source+1,a->source+2,&a->plan,QW_W4_HEAD_TENSOR_NAME);stage("MODEL_COLD_LOAD",rc,begin);storage_metric("MODEL_COLD_STORAGE");if(rc)goto cleanup;
 rc=qw_enable_w4_head(a->model,QW_W4_HEAD_TENSOR_NAME);if(rc)goto cleanup;
#ifdef QW_PARTIAL_SRAM
 volatile uint32_t *partial_sram=(volatile uint32_t *)(uintptr_t)0x10007000u;
 uint32_t partial_base=ARENA_OFF+(uint32_t)a->plan.partial_offset;
 if(partial_sram[2]!=0x50535231u||partial_sram[3]!=65536u||(partial_base&65535u)||a->plan.partial_bytes!=65536u){rc=QW_DEVICE;goto cleanup;}
 if(ag_cache_maintain_range(partial_base,65536u,10000000u)){rc=QW_UNKNOWN;goto cleanup;}
 begin=ticks();while(partial_sram[0]&2u)if(ticks()-begin>CLOCK_HZ){rc=QW_DEVICE;goto cleanup;}
 partial_sram[0]=0;partial_sram[1]=partial_base;partial_sram[0]=1;
 begin=ticks();while(partial_sram[0]&2u)if(ticks()-begin>CLOCK_HZ){rc=QW_DEVICE;goto cleanup;}
 if(partial_sram[1]!=partial_base||(partial_sram[0]&1u)==0){rc=QW_DEVICE;goto cleanup;}
 text("PARTIAL_SRAM_ENABLED");metric("base",partial_base);metric("bytes",65536u);text(" quality_approved=0\n");
#endif
 a->grants[0]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.state_offset,((uint32_t)a->plan.state_bytes+31u)&~31u,3u};
 a->grants[1]=(npu_tensor_region){ARENA_OFF+arena_bytes,(uint32_t)wsbytes,3u};
 a->grants[2]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.activation_offset,(uint32_t)a->plan.activation_bytes,3u};
 a->grants[3]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.partial_offset,(uint32_t)a->plan.partial_bytes,1u};
 a->grants[4]=(npu_tensor_region){ARENA_OFF+(uint32_t)a->plan.weights_offset,(uint32_t)a->plan.weights_bytes,1u};
 rc=npu_tensor_ag_init(&a->device,a->grants,5,1000000000u,10000000u);if(rc)goto cleanup;
 #ifdef AGENT_PROFILE_DEVICE
  rc=qw_tensor_enable(a->model,profiled_tensor,a,
#else
  rc=qw_tensor_enable(a->model,npu_tensor_model_execute,&a->device,
#endif
(void*)(uintptr_t)(ARENA_BASE+arena_bytes),wsbytes,ARENA_OFF+arena_bytes);if(rc)goto cleanup;
#if AGENT_MODEL_SMOKE_TOKENS > 0
 /* First board bring-up executes the entire model, including the full W4
  * vocabulary head, without thousands of Agent prefill tokens. No answer
  * fixture or restricted vocabulary is substituted. */
 {
  uint32_t token=16,seed=1;const float *logits=0;qw_tensor_status status;
  sd_card_reset_stats();uint64_t total=ticks();
  for(unsigned step=0;step<AGENT_MODEL_SMOKE_TOKENS;step++){
   uint64_t start=ticks();rc=qw_decode(a->model,token,&logits);
   if(rc==QW_UNKNOWN)preserve();if(rc||!logits)goto cleanup;
   uint64_t duration=ticks()-start;
   rc=qw_sample(logits,QW_VOCAB,0,1,&seed,&token);if(rc)goto cleanup;
   rc=qw_tensor_get_status(a->model,&status);if(rc)goto cleanup;
   text("NPU_MODEL_TOKEN step=");dec(step);text(" sampled=");dec(token);
   metric("decode_ticks",duration);metric("clock_hz",CLOCK_HZ);
   metric("matrix_jobs",status.raw_jobs);metric("tensor_jobs",status.jobs);
   text(" piece=[");const char *piece=0;size_t bytes=0;unsigned flags=0;
   if(!qt_piece(&a->tokenizer,token,&piece,&bytes,&flags)&&piece)span(piece,bytes);
   text("]\n");
  }
  text("NPU_MODEL_COMPLETE");metric("tokens",AGENT_MODEL_SMOKE_TOKENS);
  metric("total_ticks",ticks()-total);metric("clock_hz",CLOCK_HZ);text("\n");
  storage_metric("MODEL_WARM_STORAGE");
#ifdef AGENT_PROFILE_DEVICE
  text("MODEL_PROFILE");metric("totalstep_ticks",a->profile.totalstep_ticks);metric("quantize_ticks",a->profile.quantize_ticks);
  metric("rawexecute_ticks",a->profile.rawexecute_ticks);metric("scale_reduce_ticks",a->profile.scale_reduce_ticks);
  metric("matrix_device_cycles",a->profile.hardwarebusy_cycles);text("\n");
  for(unsigned op=1;op<23;op++)if(a->tensor_calls[op]){
   text("TENSOR_PROFILE op=");dec(op);metric("calls",a->tensor_calls[op]);metric("driver_ticks",a->tensor_driver_ticks[op]);
   metric("device_cycles",a->tensor_device_cycles[op]);text("\n");
  }
#endif
#ifndef AGENT_INTERACTIVE_UI
  goto cleanup;
#endif
 }
#endif
 const char *task_text=AGENT_TASK_TEXT;uint32_t task_key=7;
#ifdef AGENT_INTERACTIVE_UI
 ag_init(&a->saved_tools,0,0);a->task_key=7;
 for(;;){
  ag_agent_ui_stage(&a->ui,"Model ready. Choose a task or type a message.");text("AGENT_UI_READY keys=w,s,enter,q physical=K1,K2,K3,K4\n");
  for(;;){(void)ag_tick(&a->saved_tools,now_ms(0));int choice=ag_agent_ui_poll(&a->ui,&a->loop,now_ms(0));if(choice==2){(void)ag_coremark_ui_run(&a->ui,now_ms(0));continue;}if(choice==1)break;}
  rc=qw_reset(a->model);if(rc)goto cleanup;
  task_text=ag_agent_ui_task(&a->ui);task_key=++a->task_key;ag_agent_ui_begin(&a->ui);
  a->sampled_tokens=a->generated_tokens=a->generated_bytes=0;
  a->decode_intervals=a->decode_interval_ticks=a->first_token_ticks=a->last_token_ticks=0;
  a->turn_seen=0;a->loop_steps=0;a->max_step_ticks=0;qw_profile_clear(&a->profile);
#endif
 if(!qw_agent_protocol_init(&a->bridge,a->model,&a->tokenizer,&a->loop,qw_agent_profile,sizeof(qw_agent_profile)-1u,task_key,work,workbytes,a->tokens,QW_CONTEXT_MAX,a->prompt,sizeof(a->prompt),a->payload,sizeof(a->payload),a->receipt,sizeof(a->receipt),now_ms,0,0)){rc=QW_ARGUMENT;goto cleanup;}
 a->bridge.trace_user=a;a->bridge.on_token=token_event;a->bridge.on_text=generated_event;a->bridge.on_receipt=receipt_event;
#ifdef AGENT_INTERACTIVE_UI
 if(a->ui.selected==3){
  text("BEGIN_DIRECT_CHAT [");text(task_text);text("]\n");
  sd_card_reset_stats();a->task_start_ticks=ticks();
  rc=qw_agent_direct_chat(&a->bridge,task_text);
  uint64_t end=ticks();
  text("DIRECT_CHAT_STATUS rc=");integer(rc);
  generation_metrics(a,end);
  storage_metric("DIRECT_CHAT_STORAGE");
  ag_agent_ui_end(&a->ui,&a->loop);
  if(rc){ag_agent_ui_stage(&a->ui,"Direct chat failed.");text("DIRECT_CHAT_FAIL\n");}
  else ag_agent_ui_stage(&a->ui,"Done. Choose a task or type again.");
  (void)ag_agent_ui_poll(&a->ui,&a->loop,now_ms(0));
  continue;
 }
#endif
 ag_loop_init(&a->loop,qw_agent_protocol_adapter(&a->bridge),event,a,0,0);
#ifdef AGENT_INTERACTIVE_UI
 a->loop.runtime=a->saved_tools;
#endif
 if(!qw_agent_protocol_enable_cooperative(&a->bridge)){rc=QW_ARGUMENT;goto cleanup;}
 text("BEGIN_REAL_AGENT_TASK [");text(task_text);text("] original_system_bytes=");dec(sizeof(qw_agent_profile)-1u);text("\n");
 sd_card_reset_stats();
 a->task_start_ticks=ticks();
 if(ag_loop_submit(&a->loop,task_text,now_ms(0),AGENT_TIMEOUT_MS,8u)!=AG_LOOP_RUNNING){rc=QW_ARGUMENT;goto cleanup;}
 while(a->loop.status==AG_LOOP_RUNNING){
  uint64_t now=now_ms(0);
#ifdef AGENT_INTERACTIVE_UI
  if(ag_agent_ui_poll(&a->ui,&a->loop,now)<0)ag_loop_cancel(&a->loop);
#endif
  if(tensor_agent_external_ui_hook&&tensor_agent_external_ui_hook(tensor_agent_external_ui_context,&a->loop,now))ag_loop_cancel(&a->loop);
  if(a->loop.status!=AG_LOOP_RUNNING)break;
  now=now_ms(0); /* UI time must not become a stale tool execution timestamp. */
  if(ag_tick(&a->loop.runtime,now)!=AG_OK){ag_loop_cancel(&a->loop);rc=QW_IO;break;}
  /* Exactly one step per tick. No qw_agent_protocol_run blocking wrapper. */
  begin=ticks();(void)ag_loop_step(&a->loop,now);uint64_t elapsed=ticks()-begin;
  ++a->loop_steps;if(elapsed>a->max_step_ticks)a->max_step_ticks=elapsed;
 }
 uint64_t task_end=ticks();generation_metrics(a,task_end);
 text("AGENT_STATUS=");text(ag_loop_status_name(a->loop.status));text(" stage=");text(qw_agent_stage_name(a->bridge.stage));text(" model_error=");integer(a->bridge.model_error);metric("loop_steps",a->loop_steps);metric("observed_max_step_ticks",a->max_step_ticks);text(" quality_approved=0\n");
 if(a->bridge.stage==QW_AGENT_STAGE_UNKNOWN_LEASE||a->bridge.model_error==QW_UNKNOWN)preserve();
 storage_metric("AGENT_WARM_STORAGE");
 {sd_card_stats s;sd_card_get_stats(&s);if(s.read_calls){text("FAIL_WARM_AGENT_SD_READ\n");rc=QW_IO;}}
 if(!rc&&a->loop.status!=AG_LOOP_FINISHED&&a->loop.status!=AG_LOOP_CLARIFY)rc=QW_FORMAT;
#ifdef AGENT_INTERACTIVE_UI
 a->saved_tools=a->loop.runtime;ag_agent_ui_end(&a->ui,&a->loop);
 (void)ag_agent_ui_poll(&a->ui,&a->loop,now_ms(0));rc=0;
 }
#endif
cleanup:
 if(rc==QW_UNKNOWN)preserve();
 if(a->model){qw_status status;int sr=qw_get_status(a->model,&status);if(sr==QW_UNKNOWN||(!sr&&(status.lease_active||status.last_error==QW_UNKNOWN)))preserve();int close=qw_close(a->model);if(close==QW_UNKNOWN)preserve();if(!rc)rc=close;}
 text("CANDIDATE_EXIT rc=");integer(rc);text(" quality_approved=0 task_acceptance_not_asserted=1\n");return rc?20:0;
}
