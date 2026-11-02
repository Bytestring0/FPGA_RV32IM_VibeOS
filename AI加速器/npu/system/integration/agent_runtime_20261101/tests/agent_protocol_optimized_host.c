/* Standalone ACTUAL optimized software-reference tool-task candidate.
 * No HTTP, scripted tokens, board hardware, persistent SD or Flash.
 * Mixed graph: integer W4 head + Q16 LINEAR nonlinear LUTs. Norm, Delta,
 * composite decay, RoPE, body quantization/reduction remain floating point.
 * Numerical primitive validation does not approve end-to-end model quality. */
#define _POSIX_C_SOURCE 200809L
#include "qw_agent_protocol_backend.h"
#include "qw_head.h"
#include "qw_int_head.h"
#include "qw_lut_ops.h"
#include "qw_sha256.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

_Static_assert(QW_CONTEXT_MAX == 2048u, "R4 requires context2048");
_Static_assert(QW_AGENT_PROMPT_MAX == 8192u, "R4 requires prompt8192");
_Static_assert(QW_INTEGER_HEAD_NUMERIC_VERSION == 0x10000u, "frozen head version");
_Static_assert(NPU_FIXED_NUMERIC_VERSION == 0x10000u, "frozen fixed base version");
_Static_assert(QW_LUT_NUMERIC_VERSION == 0x10001u, "frozen Q16 LUT version");
static const char *const expected_hex[4] = {
    "8098c9e888efdca25cf09588037e862730c0cc9e64aceef82d23a7c3f9453532",
    "168f02aee67afee5b56edb8683c98f5fa97dae5c48dad720984a4c1ff2e5262d",
    "37d8371580043b99166df40358ec1de778d9a6dc8953598e9cebf55a5035ab30",
    "66df043b96ef2ccaa8d6e24148b0dc03eb022e8f461a88fdc527c178cbe4aa45"
};
static const char system_hex[] =
    "d2b3a9f3acb4cbc5e1ecb05a5a6cadec1b94a38bd6eb60b64768ec37e59f4e1a";
static volatile sig_atomic_t cancel_flag = 0;
static void interrupt_handler(int sig) { (void)sig; cancel_flag = 1; }
static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int hex_digest(const char *hex, unsigned char dst[32]) {
    if (strlen(hex) != 64) return 0;
    for (unsigned i = 0; i < 32; ++i) {
        int a = nibble(hex[i * 2]), b = nibble(hex[i * 2 + 1]);
        if (a < 0 || b < 0) return 0;
        dst[i] = (unsigned char)((a << 4) | b);
    }
    return 1;
}
typedef struct { FILE *file; uint64_t size; } file_source;
static int read_at(void *context, uint64_t offset, void *dst, size_t bytes) {
    file_source *s = context;
    if (offset > s->size || bytes > s->size - offset ||
        fseeko(s->file, (off_t)offset, SEEK_SET)) return -1;
    return fread(dst, 1, bytes, s->file) == bytes ? 0 : -1;
}
static int open_source(const char *name, file_source *s, npu_storage_source *out) {
    s->file = fopen(name, "rb");
    if (!s->file) return 0;
    if (fseeko(s->file, 0, SEEK_END)) { fclose(s->file); s->file = NULL; return 0; }
    off_t n = ftello(s->file);
    if (n < 0) { fclose(s->file); s->file = NULL; return 0; }
    s->size = (uint64_t)n;
    *out = (npu_storage_source){read_at, s, s->size};
    return 1;
}
static unsigned char *load_small(const char *path, size_t limit, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f || fseeko(f, 0, SEEK_END)) { if (f) fclose(f); return NULL; }
    off_t n = ftello(f);
    if (n <= 0 || (uint64_t)n > limit || fseeko(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    unsigned char *buf = malloc((size_t)n + 1u);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f); buf[n] = 0; *bytes = (size_t)n;
    return buf;
}
static uint64_t monotonic_ns(void *unused) {
    (void)unused; struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(2); }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
static uint64_t monotonic_ms(void *unused) { return monotonic_ns(unused) / UINT64_C(1000000); }
static int cancelled(void *unused) { (void)unused; return cancel_flag != 0; }
static void token_event(void *context, uint32_t pos, uint32_t id,
    float selected, const char *text, size_t bytes, int is_stop) {
    (void)context;
    printf("GENERATED position=%u id=%u selected_logit=%a stop=%d piece_hex=",
        pos, id, (double)selected, is_stop);
    for (size_t i = 0; i < bytes; ++i) printf("%02x", (unsigned char)text[i]);
    putchar('\n'); fflush(stdout);
}
static void text_event(void *context, unsigned turn, const char *suffix,
    size_t suffix_bytes, const char *payload, size_t payload_bytes,
    uint32_t stop, const char *normalized) {
    const qw_agent_protocol_backend *bridge = context;
    printf("TURN %u TURN_BOUNDARY pending_chat_end_id=%u origin=%s model_sampled_eos=%u\n",
        turn, stop, qw_agent_boundary_name(bridge->boundary_origin),
        bridge->boundary_origin == QW_AGENT_BOUNDARY_MODEL_SAMPLED_IM_END);
    printf("TURN %u MODEL_GENERATED_SUFFIX bytes=%zu BEGIN\n", turn, suffix_bytes);
    fwrite(suffix, 1, suffix_bytes, stdout);
    printf("\nTURN %u MODEL_GENERATED_SUFFIX END\n", turn);
    printf("TURN %u PROTOCOL_PAYLOAD prefix=PROVIDED bytes=%zu BEGIN\n", turn, payload_bytes);
    fwrite(payload, 1, payload_bytes, stdout);
    printf("\nTURN %u PROTOCOL_PAYLOAD END\nNORMALIZED %s\n", turn, normalized);
    fflush(stdout);
}
static void receipt_event(void *context, const char *id, const char *json) {
    (void)context; printf("REAL_RECEIPT wire=%s json=%s\n", id, json); fflush(stdout);
}
static void dump_tools(const ag_loop *loop) {
    const ag_state *s = &loop->runtime.state;
    printf("REAL_TOOL_STATE loop=%s turns=%u records=%u notes=%u timer=%d\n",
        ag_loop_status_name(loop->status), loop->turns, s->record_count,
        s->note_count, (int)s->timer);
    for (unsigned i = 0; i < s->record_count; ++i)
        printf("REAL_TOOL_RECORD index=%u id=%s tool=%s code=%s request=%s result=%s\n",
            i, s->records[i].id, s->records[i].tool,
            ag_code_name(s->records[i].receipt.code), s->records[i].request,
            s->records[i].receipt.result);
    for (unsigned i = 0; i < s->note_count; ++i)
        printf("REAL_SAVED_NOTE index=%u text=%s\n", i, s->notes[i]);
}
static void loop_event(void *context, ag_loop_event_kind kind, const char *id,
    ag_code code, const char *text) {
    printf("AG_LOOP_EVENT kind=%d wire=%s code=%s text=%s\n",
        (int)kind, id, ag_code_name(code), text);
    dump_tools(context); fflush(stdout);
}
static int successful_task(const ag_loop *loop) {
    const ag_state *s = &loop->runtime.state;
    return loop->status == AG_LOOP_FINISHED && loop->turns == 3 &&
        s->record_count == 2 && s->note_count == 1 &&
        !strcmp(s->records[0].tool, "calculate") &&
        s->records[0].receipt.code == AG_OK &&
        !strcmp(s->records[0].receipt.result, "56") &&
        !strcmp(s->records[0].id, "c7_1") &&
        !strcmp(s->records[1].tool, "save_result") &&
        !strcmp(s->records[1].id, "c7_2") &&
        s->records[1].receipt.code == AG_OK && !strcmp(s->notes[0], "56");
}
static void dump_profile(qw_model *model, int enabled) {
    qw_profile p = {0};
    int rc = qw_get_profile(model, &p);
    printf("PROFILE enabled=%d rc=%d clock=CLOCK_MONOTONIC unit=ns scope=task clear_before_task=1\n", enabled, rc);
    if (rc) return;
    printf("PROFILE_NS totalstep=%" PRIu64 " quantize=%" PRIu64 " rawexecute=%" PRIu64
        " scale_reduce=%" PRIu64 " bf16head=%" PRIu64 "\n",
        p.totalstep_ticks, p.quantize_ticks, p.rawexecute_ticks, p.scale_reduce_ticks, p.bf16head_ticks);
    printf("PROFILE_COUNTS tokens_started=%" PRIu64 " tokens_completed=%" PRIu64
        " tokens_failed=%" PRIu64 " matrix_calls=%" PRIu64 " jobs_started=%" PRIu64
        " jobs_completed=%" PRIu64 " bf16head_calls=%" PRIu64 "\n",
        p.tokens_started, p.tokens_completed, p.tokens_failed, p.matrix_calls,
        p.jobs_started, p.jobs_completed, p.bf16head_calls);
    printf("PROFILE_HARDWARE unavailable=1 software_reference=1 hardwarebusy_jobs=%" PRIu64
        " hardwarebusy_cycles=%" PRIu64 "\n", p.hardwarebusy_jobs, p.hardwarebusy_cycles);
}

int main(int argc, char **argv) {
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "Usage: optimized_agent_protocol_host HEADW4.BIN CPU_RAW.BIN TENSOR_HEAD.JSON TOKEN.BIN protocol-r4-original-system.txt [timeout_ms]\nOptional environment: QW_HOST_PROFILE=0 disables runtime profiling (default 1).\n");
        return 2;
    }
    uint64_t host_start = monotonic_ns(NULL);
    uint32_t timeout_ms = 43200000u;
    if (argc == 7) {
        char *end; unsigned long n = strtoul(argv[6], &end, 10);
        if (!argv[6][0] || *end || !n || n > UINT32_MAX) return 2;
        timeout_ms = (uint32_t)n;
    }
    const char *profile_env = getenv("QW_HOST_PROFILE");
    if (profile_env && strcmp(profile_env, "0") && strcmp(profile_env, "1")) {
        fprintf(stderr, "QW_HOST_PROFILE must be 0 or 1\n"); return 2;
    }
    int profile_enabled = !profile_env || !strcmp(profile_env, "1");
    printf("CANDIDATE mode=SOFTWARE_REFERENCE quality=NOT_APPROVED context=%u prompt=%u reserved_prefix=%u tile_requested=%u\n",
        QW_CONTEXT_MAX, QW_AGENT_PROMPT_MAX, 16u << 20, QW_TILE_ROWS_OPTIMIZED);
    printf("NUMERICS fixed_base=0x%08x integer_head=0x%08x nonlinear_lut=0x%08x lut_mode=1 interpolation=LINEAR greedy=integer_bits graph=MIXED remaining_float=Norm,Delta,composite_decay,RoPE,body_quantize_reduce\n",
        NPU_FIXED_NUMERIC_VERSION, QW_INTEGER_HEAD_NUMERIC_VERSION, QW_LUT_NUMERIC_VERSION);
    signal(SIGINT, interrupt_handler);
    unsigned char sha[4][32];
    for (unsigned i = 0; i < 4; ++i) {
        if (!hex_digest(expected_hex[i], sha[i])) return 2;
        printf("SOURCE %s expected_sha256=%s\n", argv[i + 1u], expected_hex[i]);
    }
    file_source files[3] = {{0}}; npu_storage_source src[3];
    for (unsigned i = 0; i < 3; ++i)
        if (!open_source(argv[i + 1u], &files[i], &src[i])) { perror(argv[i + 1u]); return 2; }
    size_t token_bytes, system_bytes;
    unsigned char *blob = load_small(argv[4], 16000000u, &token_bytes);
    char *profile = (char *)load_small(argv[5], QW_AGENT_PROMPT_MAX, &system_bytes);
    if (!blob || !profile) { fprintf(stderr, "TOKEN/profile read or bound failed\n"); return 2; }
    /* Same universal-newline canonicalization and final-newline removal as baseline. */
    size_t normalized_bytes = 0;
    for (size_t i = 0; i < system_bytes; ++i) {
        if (profile[i] == '\r') {
            if (i + 1u < system_bytes && profile[i + 1u] == '\n') ++i;
            profile[normalized_bytes++] = '\n';
        } else profile[normalized_bytes++] = profile[i];
    }
    system_bytes = normalized_bytes; profile[system_bytes] = 0;
    while (system_bytes && profile[system_bytes - 1u] == '\n') profile[--system_bytes] = 0;
    unsigned char expected_system[32], actual_system[32]; qw_sha256 hash;
    if (!hex_digest(system_hex, expected_system)) return 2;
    qw_sha_init(&hash); qw_sha_update(&hash, profile, system_bytes); qw_sha_final(&hash, actual_system);
    if (system_bytes != 6652u || memcmp(expected_system, actual_system, 32u)) {
        fprintf(stderr, "Original complete R4 system byte/hash mismatch bytes=%zu\n", system_bytes); return 2;
    }
    printf("SYSTEM canonical_bytes=%zu sha256=%s complete_R4=1\n", system_bytes, system_hex);
    qw_tokenizer tokenizer; int rc = qt_open(&tokenizer, blob, token_bytes, sha[3]);
    printf("TOKEN_OPEN rc=%d bytes=%zu profile=%s profile_bytes=%zu\n",
        rc, token_bytes, QW_AGENT_PROTOCOL_VERSION, system_bytes); fflush(stdout);
    if (rc) return 1;
    uint64_t load_end = monotonic_ns(NULL);
    qw_profile runtime_profile = {0}; qw_options options = {0};
    options.context_tokens = QW_CONTEXT_MAX; options.prefill_chunk_tokens = 128u;
    options.tile_rows = 512u;
    options.arena_ddr_offset = 16u << 20; options.system_reserved_bytes = 16u << 20;
    options.mode = NPU_RAW_SOFTWARE_REFERENCE;
    options.budgets = (npu_raw_budgets){10000000u, 1000000u};
    options.weight_sha256 = sha[0]; options.cpu_sha256 = sha[1]; options.catalog_sha256 = sha[2];
    if (profile_enabled) { options.profile = &runtime_profile; options.profile_clock = monotonic_ns; }
    size_t capacity = NPU_RAW_DDR_LIMIT - options.arena_ddr_offset;
    qw_memory_plan plan;
    rc = qw_try_opt_tile(&options, src[0].size_bytes, src[1].size_bytes, src[2].size_bytes, capacity, &plan);
    printf("PLAN rc=%d arena=%zu capacity=%zu tile_rows=%u mode=SOFTWARE_REFERENCE\n",
        rc, rc ? 0u : plan.arena_bytes, capacity, options.tile_rows); fflush(stdout);
    if (rc || options.tile_rows != QW_TILE_ROWS_OPTIMIZED || plan.arena_bytes > SIZE_MAX - 31u) return 1;
    size_t arena_bytes = (plan.arena_bytes + 31u) & ~(size_t)31u;
    if (arena_bytes > capacity) return 1;
    printf("MEMORY arena_ddr_offset=%u system_reserved=%zu weights=%zu cpu=%zu catalog=%zu state=%zu work=%zu activation=%zu partial=%zu logits=%zu arena_allocated=%zu\n",
        options.arena_ddr_offset, plan.system_reserved_bytes, plan.weights_bytes,
        plan.cpu_bytes, plan.catalog_bytes, plan.state_bytes, plan.work_bytes,
        plan.activation_bytes, plan.partial_bytes, plan.logits_bytes, arena_bytes);
    unsigned char *arena = aligned_alloc(32u, arena_bytes);
    if (!arena) { perror("aligned_alloc"); return 1; }
    uint64_t open_start = monotonic_ns(NULL);
    qw_model *model = NULL;
    rc = qw_open_w4_head_candidate(&model, arena, arena_bytes, &options,
        &src[0], &src[1], &src[2], &plan, QW_W4_HEAD_TENSOR_NAME);
    printf("OPEN rc=%d mode=SOFTWARE_REFERENCE candidate_tensor=%s\n", rc, QW_W4_HEAD_TENSOR_NAME); fflush(stdout);
    if (rc || !model) return 1;
    rc = qw_enable_w4_head(model, QW_W4_HEAD_TENSOR_NAME);
    printf("ENABLE_W4_HEAD rc=%d exact_tensor=%s\n", rc, QW_W4_HEAD_TENSOR_NAME);
    if (rc) return 1;
    rc = qw_enable_integer_w4_head(model);
    printf("ENABLE_INTEGER_HEAD rc=%d\n", rc);
    if (rc) return 1;
    rc = qw_set_lut_nonlinear(model, 1u);
    printf("ENABLE_LUT rc=%d mode=1 position_required=0\n", rc);
    if (rc) return 1;
    qw_integer_head_status head_status = {0}; qw_lut_status lut_status = {0};
    qw_status status = {0}; const char *head_name = NULL;
    if (qw_get_status(model, &status) || status.position || status.needs_reset || status.lease_active ||
        qw_get_w4_head(model, &head_name) || !head_name || strcmp(head_name, QW_W4_HEAD_TENSOR_NAME) ||
        qw_get_integer_w4_head(model, &head_status) || !head_status.enabled ||
        qw_get_lut_status(model, &lut_status) || !lut_status.enabled) {
        fprintf(stderr, "Optimized mode verification failed at position zero\n"); return 1;
    }
    uint64_t open_end = monotonic_ns(NULL);
    size_t workspace_bytes = qt_workspace_bytes(QW_AGENT_PROMPT_MAX);
    void *workspace = malloc(workspace_bytes);
    uint32_t *tokens = malloc(QW_CONTEXT_MAX * sizeof(*tokens));
    char *prompt = malloc(QW_AGENT_PROMPT_MAX);
    char *protocol_payload = calloc(QW_TOOL_TEXT_MAX + 1u, 1u);
    char *receipt = malloc(AG_LOOP_RESPONSE_MAX + 1u);
    if (!workspace || !tokens || !prompt || !protocol_payload || !receipt) {
        fprintf(stderr, "Agent scratch allocation failed\n"); return 1;
    }
    printf("HOST_SCRATCH tokenizer_workspace=%zu tokens=%zu prompt=%u payload=%u receipt=%u tokenizer_blob=%zu system=%zu\n",
        workspace_bytes, QW_CONTEXT_MAX * sizeof(*tokens), QW_AGENT_PROMPT_MAX,
        QW_TOOL_TEXT_MAX + 1u, AG_LOOP_RESPONSE_MAX + 1u, token_bytes, system_bytes);
    ag_loop loop; qw_agent_protocol_backend bridge;
    if (!qw_agent_protocol_init(&bridge, model, &tokenizer, &loop,
            profile, system_bytes, 7u, workspace, workspace_bytes,
            tokens, QW_CONTEXT_MAX, prompt, QW_AGENT_PROMPT_MAX,
            protocol_payload, QW_TOOL_TEXT_MAX + 1u, receipt, AG_LOOP_RESPONSE_MAX + 1u,
            monotonic_ms, cancelled, NULL)) {
        fprintf(stderr, "BIND FAILED stage=%s model_error=%d\n", qw_agent_stage_name(bridge.stage), bridge.model_error); return 1;
    }
    bridge.on_token = token_event; bridge.on_text = text_event;
    bridge.on_receipt = receipt_event; bridge.trace_user = &bridge;
    ag_loop_init(&loop, qw_agent_protocol_adapter(&bridge), loop_event, &loop, NULL, NULL);
    const char task[] = "计算7*8保存";
    printf("BEGIN real model task=%s timeout_ms=%u max_turns=8 tool_executor=actual_CPU commit=volatile quality=NOT_APPROVED\n", task, timeout_ms);
    fflush(stdout);
    qw_profile_clear(&runtime_profile);
    uint64_t task_start = monotonic_ns(NULL);
    ag_loop_status result = qw_agent_protocol_run(&bridge, task, timeout_ms, 8u);
    uint64_t task_end = monotonic_ns(NULL);
    int status_rc = qw_get_status(model, &status);
    printf("RESULT loop=%s stage=%s native_model_error=%d normalize_error=%d\n",
        ag_loop_status_name(result), qw_agent_stage_name(bridge.stage), bridge.model_error, (int)bridge.norm_error);
    printf("MODEL status_rc=%d position=%u lease_active=%u needs_reset=%u last_error=%d\n",
        status_rc, status.position, status.lease_active, status.needs_reset, status.last_error);
    printf("BOUNDARY origin=%s canonical_im_end_id=%u pending_chat_end_id=%u pending=%u\n",
        qw_agent_boundary_name(bridge.boundary_origin), bridge.canonical_im_end, bridge.stop_token, bridge.stop_pending);
    printf("TOOL records=%u notes=%u final_text=%s\n", loop.runtime.state.record_count, loop.runtime.state.note_count, loop.pending_text);
    dump_tools(&loop);
    printf("RAW_FINAL_PROTOCOL_PAYLOAD prefix=PROVIDED bytes=%zu BEGIN\n%s\nRAW_FINAL_PROTOCOL_PAYLOAD END\n", strlen(protocol_payload), protocol_payload);
    dump_profile(model, profile_enabled);
    int head_rc = qw_get_integer_w4_head(model, &head_status);
    int lut_rc = qw_get_lut_status(model, &lut_status);
    printf("INTEGER_HEAD rc=%d enabled=%u version=0x%08x numeric_errors=%u saturations=%u underflows=%u\n",
        head_rc, head_status.enabled, head_status.numeric_version, head_status.numeric_errors, head_status.saturations, head_status.underflows);
    printf("LUT rc=%d enabled=%u version=0x%08x status=0x%x domain_clamps=%u saturations=%u underflows=%u invalid_inputs=%u zero_vectors=%u sigmoid=%" PRIu64 " silu=%" PRIu64 " softplus=%" PRIu64 " exp_negative=%" PRIu64 "\n",
        lut_rc, lut_status.enabled, lut_status.numeric_version, lut_status.fixed.status,
        lut_status.fixed.domain_clamps, lut_status.fixed.saturations, lut_status.fixed.underflows,
        lut_status.fixed.invalid_inputs, lut_status.fixed.zero_vectors, lut_status.sigmoid_calls,
        lut_status.silu_calls, lut_status.softplus_calls, lut_status.exp_negative_calls);
    printf("HOST_TIMING_NS load_and_tokenizer=%" PRIu64 " plan_allocate=%" PRIu64 " open_and_modes=%" PRIu64 " bind=%" PRIu64 " task=%" PRIu64 " before_close=%" PRIu64 "\n",
        load_end - host_start, open_start - load_end, open_end - open_start,
        task_start - open_end, task_end - task_start, monotonic_ns(NULL) - host_start);
    int success = successful_task(&loop) && bridge.stage == QW_AGENT_STAGE_OK &&
        !status_rc && !status.needs_reset && !head_rc && head_status.enabled &&
        !head_status.numeric_errors && !lut_rc && lut_status.enabled;
    if (status_rc || status.lease_active || bridge.stage == QW_AGENT_STAGE_UNKNOWN_LEASE) {
        fprintf(stderr, "UNKNOWN lease: arena/model intentionally NOT closed or reused\n"); return 1;
    }
    rc = qw_close(model);
    if (rc) { fprintf(stderr, "CLOSE rc=%d: arena retained\n", rc); return 1; }
    free(arena); free(receipt); free(protocol_payload); free(prompt); free(tokens);
    free(workspace); free(profile); free(blob);
    for (unsigned i = 0; i < 3; ++i) fclose(files[i].file);
    printf("HOST_TIMING_NS total=%" PRIu64 "\n", monotonic_ns(NULL) - host_start);
    printf("QUALITY scope=one_real_tool_task task_pass=%u model_quality=NOT_APPROVED\n", (unsigned)success);
    printf("AGENT_PROTOCOL_REAL_MODEL_%s\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
