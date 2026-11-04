/* Dedicated CPU + NPU optimization proof. Experimental W4 quality is unapproved.
 * This remains the current mixed FP32/BF16/W4 graph. An explicit build flag
 * can select integer output-head arithmetic and integer LUT sigmoid/SiLU/
 * softplus/softmax-exp. Recurrent math and normalization still use FP32;
 * this is not an entire integer graph.
 * No Agent loop, tokenizer, canned logits, SD writes, or recovery after UNKNOWN.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "qw_runtime.h"
#include "qw_head.h"
#include "qw_int_head.h"
#include "qw_lut_ops.h"
#include "qw_libm.h"
#include "npu_cpu_dma_opt.h"
#include "npu_fat32.h"
#include "sd_card.h"
#include "npu_opt_deployment.h"

#define BOARD_CPU_HZ 81000000u
#define SD_POLLS 400000u
#define DDR_LIMIT UINT32_C(0xc0000000)
#define ARENA_BASE UINT32_C(0x81000000)
#define ARENA_OFF UINT32_C(0x01000000)
#define ARENA_CAP UINT32_C(0x3f000000)
#define INPUT_TOKEN 16u
#ifndef NPU_OPT_INTEGER_HEAD
#define NPU_OPT_INTEGER_HEAD 0
#endif
#ifndef NPU_OPT_LUT_NONLINEAR
#define NPU_OPT_LUT_NONLINEAR 0
#endif
#define UART (*(volatile uint32_t *)(uintptr_t)0x10000000u)
#define DDR_STATUS (*(volatile uint32_t *)(uintptr_t)0x10000200u)

/* Persistent application data, including FAT buffers, options and both profile
 * snapshots, lives in the otherwise unused DDR tail, outside plan.arena_bytes.
 * Only bounded call frames and immutable firmware constants use TCM.
 */
typedef struct {
    npu_fat32_fs filesystem;
    npu_fat32_file files[3];
    npu_storage_source sources[3];
    qw_model *model;
    qw_options options;
    qw_memory_plan plan;
    qw_profile live, baseline, optimized;
    qw_status status;
    qw_integer_head_status integer_status;
    qw_lut_status baseline_lut, optimized_lut;
    const float *logits;
    uint32_t baseline_argmax, optimized_argmax;
    float baseline_best, optimized_best;
    float reference_squared_l2, error_squared_l2, relative_l2, maxabs;
    float baseline_logits[QW_VOCAB];
} opt_application;
#define APP_BYTES ((sizeof(opt_application)+31u)&~(size_t)31u)
#define APP_BASE (DDR_LIMIT-(uint32_t)APP_BYTES)
_Static_assert(QW_CONTEXT_MAX == 2048u,"Dedicated proof requires context 2048");
_Static_assert(sizeof(float) == 4u,"FP32 benchmark required");
_Static_assert(APP_BYTES < ARENA_CAP,"Application must fit remaining DDR");

static void text(const char *s){while(*s)UART=(uint8_t)*s++;}
static void span(const char *s,size_t n){while(n--)UART=(uint8_t)*s++;}
static void dec(uint32_t n){char b[10];unsigned k=0;do{b[k++]=(char)('0'+n%10u);n/=10u;}while(n);while(k)UART=(uint8_t)b[--k];}
static void integer(int n){if(n<0){text("-");dec(0u-(uint32_t)n);}else dec((uint32_t)n);}
static void hex(uint32_t v){const char *d="0123456789abcdef";for(int i=7;i>=0;i--)UART=(uint8_t)d[(v>>(i*4))&15u];}
static void wide(uint64_t n){text("0x");hex((uint32_t)(n>>32));hex((uint32_t)n);}
static uint32_t bits(float f){union{float f;uint32_t u;}v;v.f=f;return v.u;}
static int finite(float f){return(bits(f)&UINT32_C(0x7f800000))!=UINT32_C(0x7f800000);}
/* Stable integer-only mtime read. No FP conversion or wall-clock assumption.
 * Runtime ticks are this 81MHz CPU timer domain; device busy has its own CSR
 * clock domain and must not be subtracted from these ticks.
 */
static uint64_t ticks(void){
    volatile const uint32_t *lo=(volatile const uint32_t *)(uintptr_t)0x10000100u;
    volatile const uint32_t *hi=(volatile const uint32_t *)(uintptr_t)0x10000104u;
    uint32_t a,b,l;do{a=*hi;l=*lo;b=*hi;}while(a!=b);return((uint64_t)a<<32)|l;
}
static uint64_t profile_clock(void *user){(void)user;return ticks();}
static void stage(const char *s,int rc,uint64_t begin){text("STAGE ");text(s);text(" rc=");integer(rc);text(" mtime_ticks=");wide(ticks()-begin);text("\n");}
static void preserve(void){text("UNKNOWN preserve_context=1 preserve_arena=1 preserve_application=1 no_recovery=1\n");for(;;)__asm__ volatile("nop":::"memory");}
static int sd_blocks(void *ctx,uint32_t lba,uint32_t count,void *dst){(void)ctx;return sd_card_read_blocks(SD_SPI_CSR_BASE,lba,count,dst,SD_POLLS);}
static void metric(const char *name,uint64_t value){text(" ");text(name);text("=");wide(value);}
static void profile_record(const char *name,const qw_profile *p){
    text("PROFILE path=");text(name);
    metric("totalstep_ticks",p->totalstep_ticks);metric("quantize_ticks",p->quantize_ticks);
    metric("rawexecute_ticks",p->rawexecute_ticks);metric("scale_reduce_ticks",p->scale_reduce_ticks);
    metric("bf16head_ticks",p->bf16head_ticks);metric("hardwarebusy_cycles",p->hardwarebusy_cycles);
    metric("tokens_started",p->tokens_started);metric("tokens_completed",p->tokens_completed);
    metric("tokens_failed",p->tokens_failed);metric("matrix_calls",p->matrix_calls);
    metric("jobs_started",p->jobs_started);metric("jobs_completed",p->jobs_completed);
    metric("hardwarebusy_jobs",p->hardwarebusy_jobs);metric("bf16head_calls",p->bf16head_calls);text("\n");
}
static void lut_record(const char *name,const qw_lut_status *s){
    text("LUT_STATUS path=");text(name);text(" enabled=");dec(s->enabled);
    text(" numeric_version=0x");hex(s->numeric_version);text(" fixed_status=0x");hex(s->fixed.status);
    metric("domain_clamps",s->fixed.domain_clamps);metric("saturations",s->fixed.saturations);
    metric("underflows",s->fixed.underflows);metric("invalid_inputs",s->fixed.invalid_inputs);
    metric("zero_vectors",s->fixed.zero_vectors);metric("sigmoid_calls",s->sigmoid_calls);
    metric("silu_calls",s->silu_calls);metric("softplus_calls",s->softplus_calls);
    metric("exp_negative_calls",s->exp_negative_calls);text("\n");
}
static int scan_logits(const float *logits,uint32_t *best,float *value){
    if(!logits)return QW_NUMERIC;*best=0;*value=logits[0];
    for(uint32_t i=0;i<QW_VOCAB;i++){if(!finite(logits[i]))return QW_NUMERIC;if(logits[i]>*value){*value=logits[i];*best=i;}}
    return QW_OK;
}
static void logit_record(const char *name,const float *logits,uint32_t best,float value){
    uint32_t hash=UINT32_C(2166136261);
    for(uint32_t i=0;i<QW_VOCAB;i++){uint32_t v=bits(logits[i]);for(unsigned b=0;b<4;b++){hash^=v&255u;hash*=UINT32_C(16777619);v>>=8;}}
    text("ACTUAL_LOGITS path=");text(name);text(" input_token=");dec(INPUT_TOKEN);
    text(" count=");dec(QW_VOCAB);text(" argmax=");dec(best);
    text(" argmax_logit_bits=0x");hex(bits(value));text(" input_token_logit_bits=0x");hex(bits(logits[INPUT_TOKEN]));
    text(" all_logits_le_f32_fnv1a32=0x");hex(hash);text("\n");
}
static int run_one(opt_application *app,int optimized){
    const char *name=optimized?(NPU_OPT_INTEGER_HEAD?"EXPERIMENTAL_INTEGER_W4_TILE8192":"EXPERIMENTAL_FP32_W4_TILE8192"):"CPU_OPT_BF16_ORACLE_TILE512";
    qw_profile *snapshot=optimized?&app->optimized:&app->baseline;
    text("BEGIN ACTUAL_ONE_TOKEN path=");text(name);text(" token=16\n");
    uint64_t begin=ticks();int rc=qw_decode(app->model,INPUT_TOKEN,&app->logits);stage(name,rc,begin);
    if(rc==QW_UNKNOWN)preserve();
    int pr=qw_get_profile(app->model,snapshot);if(!pr)profile_record(name,snapshot);
    qw_lut_status *lut=optimized?&app->optimized_lut:&app->baseline_lut;
    int lr=qw_get_lut_status(app->model,lut);if(!lr)lut_record(name,lut);
    if(rc)return rc;if(pr)return pr;if(lr)return lr;
    if(lut->enabled!=(uint32_t)(optimized&&NPU_OPT_LUT_NONLINEAR)||lut->numeric_version!=QW_LUT_NUMERIC_VERSION)return QW_FORMAT;
    if((lut->fixed.status&(NPU_FIXED_STATUS_ERROR_MASK|NPU_FIXED_STATUS_SATURATED))||lut->fixed.saturations||lut->fixed.invalid_inputs)return QW_NUMERIC;
    if(lut->enabled){
        if(!lut->sigmoid_calls||!lut->silu_calls||!lut->softplus_calls||!lut->exp_negative_calls)return QW_FORMAT;
    }else if(lut->sigmoid_calls||lut->silu_calls||lut->softplus_calls||lut->exp_negative_calls||lut->fixed.status)return QW_FORMAT;
    if(optimized&&NPU_OPT_INTEGER_HEAD){
        rc=qw_get_integer_w4_head(app->model,&app->integer_status);if(rc)return rc;
        text("INTEGER_HEAD_STATUS enabled=");dec(app->integer_status.enabled);
        text(" numeric_version=0x");hex(app->integer_status.numeric_version);
        text(" numeric_errors=");dec(app->integer_status.numeric_errors);
        text(" saturations=");dec(app->integer_status.saturations);
        text(" underflows=");dec(app->integer_status.underflows);text("\n");
        if(!app->integer_status.enabled||app->integer_status.numeric_errors||app->integer_status.saturations)return QW_NUMERIC;
    }
    rc=qw_get_status(app->model,&app->status);if(rc)return rc;
    if(app->status.lease_active)preserve();
    if(app->status.position!=1u||app->status.needs_reset||app->status.last_error||
       app->status.logits!=app->logits||app->status.logits_count!=QW_VOCAB)return QW_FORMAT;
    /* Counts refer to this frozen 24-layer graph. They prove both complete
     * real paths, including 31 W4 output-head jobs in the candidate path. */
    if(snapshot->tokens_started!=1u||snapshot->tokens_completed!=1u||snapshot->tokens_failed||
       snapshot->matrix_calls!=(optimized?187u:186u)||snapshot->jobs_started!=(optimized?217u:816u)||
       snapshot->jobs_completed!=snapshot->jobs_started||snapshot->hardwarebusy_jobs!=snapshot->jobs_completed||
       snapshot->bf16head_calls!=(optimized?0u:1u))return QW_FORMAT;
    uint32_t *best=optimized?&app->optimized_argmax:&app->baseline_argmax;
    float *value=optimized?&app->optimized_best:&app->baseline_best;
    rc=scan_logits(app->logits,best,value);if(rc)return rc;
    logit_record(name,app->logits,*best,*value);return QW_OK;
}
/* FP32 reference/error work is benchmark-only and outside runtime profiling.
 * No threshold is invented: even an argmax match does not approve quality. */
static int compare_quality(opt_application *app){
    float reference=0.0f,error=0.0f,maximum=0.0f;
    for(uint32_t i=0;i<QW_VOCAB;i++){
        float base=app->baseline_logits[i],candidate=app->logits[i];
        if(!finite(base)||!finite(candidate))return QW_NUMERIC;
        float delta=candidate-base,absolute=delta<0.0f?-delta:delta;
        reference=reference+base*base;error=error+delta*delta;if(absolute>maximum)maximum=absolute;
    }
    if(!finite(reference)||!finite(error)||!finite(maximum))return QW_NUMERIC;
    float relative=reference>0.0f?sqrtf(error/reference):(error==0.0f?0.0f:INFINITY);
    if(!finite(relative))return QW_NUMERIC;
    app->reference_squared_l2=reference;app->error_squared_l2=error;app->relative_l2=relative;app->maxabs=maximum;
    text("QUALITY_MEASURED fp32_benchmark_only=1 relative_l2_bits=0x");hex(bits(relative));
    text(" maxabs_bits=0x");hex(bits(maximum));text(" reference_squared_l2_bits=0x");hex(bits(reference));
    text(" error_squared_l2_bits=0x");hex(bits(error));text(" argmax_equal=");dec(app->baseline_argmax==app->optimized_argmax);
    text(" baseline_argmax=");dec(app->baseline_argmax);text(" candidate_argmax=");dec(app->optimized_argmax);
    text(" candidate_at_baseline_argmax_bits=0x");hex(bits(app->logits[app->baseline_argmax]));
    text(" baseline_at_candidate_argmax_bits=0x");hex(bits(app->baseline_logits[app->optimized_argmax]));
    text("\nQUALITY_STATUS=EXPERIMENTAL_W4_UNAPPROVED agent_threshold=UNSET quality_pass_claimed=0\n");return QW_OK;
}
int main(void){
    text("NPU_OPT_BOARD_20261102 actual_CPU_NPU_optimization_proof\n");
    text("CLOCK=integer_mtime hz=81000000 hardwarebusy_clock=separate_NPU_CSR\n");
    text("CONTEXT=2048 ARENA_BASE=0x81000000 ARENA_CAP=0x3f000000 SYSTEM_RESERVED=4194304 FRAMEBUFFER_RESERVED=12582912\n");
    text("GRAPH=mixed_FP32_BF16_W4 entire_integer_graph=0 recurrent_norm_float=1 decay_exp_float=1\n");
    text("CANDIDATE_INTEGER_HEAD=");dec(NPU_OPT_INTEGER_HEAD);
    text(" CANDIDATE_LUT_NONLINEAR=");dec(NPU_OPT_LUT_NONLINEAR);text(" BF16_BASELINE=FP32_NUMERICAL_ORACLE baseline_lut=0\n");
    text("QUALITY_STATUS=EXPERIMENTAL_W4_UNAPPROVED agent_threshold=UNSET\n");
    uint64_t begin=ticks();
    while(!(DDR_STATUS&1u)){
        if(DDR_STATUS&0x12u){stage("DDR_FAULT",QW_DEVICE,begin);return 1;}
        if(ticks()-begin>(uint64_t)BOARD_CPU_HZ*15u){stage("DDR_TIMEOUT",QW_DEVICE,begin);return 2;}
    }
    stage("DDR_READY",QW_OK,begin);
    opt_application *app=(opt_application *)(uintptr_t)APP_BASE;memset(app,0,sizeof(*app));
    text("APPLICATION_DDR_BASE=0x");hex(APP_BASE);text(" APPLICATION_BYTES=");dec((uint32_t)APP_BYTES);text(" BASELINE_LOGITS_BYTES=");dec(sizeof(app->baseline_logits));text("\n");
    begin=ticks();int rc=sd_card_init(SD_SPI_CSR_BASE,SD_POLLS);stage("SD_INIT",rc,begin);if(rc)goto cleanup;
    begin=ticks();rc=npu_fat32_mount(&app->filesystem,sd_blocks,NULL,0);stage("FAT32_MOUNT",rc,begin);if(rc)goto cleanup;
    for(unsigned i=0;i<3;i++){
        text("SD_EXACT_SHORT_NAME=[");span(opt_sd_names[i],11);text("]\n");
        begin=ticks();rc=npu_fat32_open(&app->filesystem,opt_sd_names[i],&app->files[i]);stage("FAT32_OPEN",rc,begin);if(rc)goto cleanup;
        text("FILE bytes=");wide(app->files[i].size_bytes);text("\n");
        if(app->files[i].size_bytes!=opt_file_sizes[i]){rc=QW_FORMAT;goto cleanup;}
        app->sources[i]=(npu_storage_source){npu_fat32_read_exact_at,&app->files[i],app->files[i].size_bytes};
    }
    app->options.context_tokens=2048;app->options.prefill_chunk_tokens=128;app->options.tile_rows=512;
    app->options.arena_ddr_offset=ARENA_OFF;app->options.system_reserved_bytes=4194304;
    app->options.framebuffer_reserved_bytes=12582912;app->options.mode=NPU_RAW_HARDWARE;
    app->options.device=npu_dma_mmio();app->options.budgets=(npu_raw_budgets){100000000u,1000000u};
    app->options.weight_sha256=opt_weight_sha;app->options.cpu_sha256=opt_cpu_sha;app->options.catalog_sha256=opt_catalog_sha;
    app->options.profile=&app->live;app->options.profile_clock=profile_clock;
    app->options.bf16_head_kernel=npu_cpu_bf16_head;
    app->options.lut_nonlinear=0;
    rc=qw_try_opt_tile(&app->options,app->sources[0].size_bytes,app->sources[1].size_bytes,
                       app->sources[2].size_bytes,ARENA_CAP,&app->plan);
    if(rc)goto cleanup;
    if(app->options.tile_rows!=8192u||app->plan.partial_bytes!=8192u*256u||
       app->plan.arena_bytes>(size_t)(APP_BASE-ARENA_BASE)){rc=QW_CAPACITY;goto cleanup;}
    text("PLAN arena_bytes=");dec((uint32_t)app->plan.arena_bytes);text(" partial_bytes=");dec((uint32_t)app->plan.partial_bytes);
    text(" remaining_before_application=");dec(APP_BASE-ARENA_BASE-(uint32_t)app->plan.arena_bytes);text("\n");
    text("BEGIN QW_OPEN_W4_HEAD_CANDIDATE load_once=1 default_head=BF16\n");begin=ticks();
    rc=qw_open_w4_head_candidate(&app->model,(void *)(uintptr_t)ARENA_BASE,ARENA_CAP,&app->options,
                                &app->sources[0],&app->sources[1],&app->sources[2],&app->plan,QW_W4_HEAD_TENSOR_NAME);
    stage("QW_OPEN_W4_HEAD_CANDIDATE",rc,begin);if(rc)goto cleanup;
    const char *head=NULL;rc=qw_get_w4_head(app->model,&head);if(rc)goto cleanup;
    if(head){rc=QW_FORMAT;goto cleanup;}
    rc=qw_set_tile_rows(app->model,512);if(rc)goto cleanup;
    qw_profile_clear(&app->live);rc=run_one(app,0);if(rc)goto cleanup;
    memcpy(app->baseline_logits,app->logits,sizeof(app->baseline_logits));
    begin=ticks();rc=qw_reset(app->model);stage("QW_RESET",rc,begin);if(rc)goto cleanup;
    qw_profile_clear(&app->live);rc=qw_set_tile_rows(app->model,8192);if(rc)goto cleanup;
    rc=qw_set_lut_nonlinear(app->model,NPU_OPT_LUT_NONLINEAR?1u:0u);if(rc)goto cleanup;
    text("CANDIDATE_LUT_POLICY enabled=");dec(NPU_OPT_LUT_NONLINEAR);text(" quality_approved=0\n");
    rc=qw_enable_w4_head(app->model,QW_W4_HEAD_TENSOR_NAME);if(rc)goto cleanup;
    rc=qw_get_w4_head(app->model,&head);if(rc)goto cleanup;
    if(!head||strcmp(head,QW_W4_HEAD_TENSOR_NAME)){rc=QW_FORMAT;goto cleanup;}
    text("W4_HEAD_EXPLICITLY_ENABLED tensor=");text(head);text(" quality_approved=0\n");
    if(NPU_OPT_INTEGER_HEAD){
        rc=qw_enable_integer_w4_head(app->model);if(rc)goto cleanup;
        text("INTEGER_W4_HEAD_EXPLICITLY_ENABLED quality_approved=0\n");
    }
    rc=run_one(app,1);if(rc)goto cleanup;rc=compare_quality(app);
cleanup:
    if(rc==QW_UNKNOWN)preserve();
    if(app->model){
        int sr=qw_get_status(app->model,&app->status);
        if(sr){if(!rc)rc=sr;}else{
            text("MODEL_STATUS position=");dec(app->status.position);text(" npu_jobs=");dec(app->status.npu_jobs);
            text(" lease=");dec(app->status.lease_active);text("\n");if(app->status.lease_active)preserve();
        }
        begin=ticks();int close=qw_close(app->model);stage("QW_CLOSE",close,begin);
        if(close==QW_UNKNOWN)preserve();if(!close)app->model=NULL;if(!rc&&close)rc=close;
    }
    if(!rc)text("PASS_OPT_PIPELINE execution_finite_valid=1 actual_tokens=2 baseline_tile=512 candidate_tile=8192 close=1 quality_approved=0\n");
    else{text("FAIL_OPT_PIPELINE rc=");integer(rc);text(" quality_approved=0\n");}
    return rc?3:0;
}
