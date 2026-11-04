#ifndef QW_AGENT_PROTOCOL_BACKEND_H
#define QW_AGENT_PROTOCOL_BACKEND_H

#include "ag_loop.h"
#include "qw_tool_normalize.h"
#include "../model/qw_runtime.h"
#include "../model/qw_tokenizer.h"
#ifdef QW_ENABLE_TENSOR_V2
#include "../model/qw_tensor_async.h"
#endif

/* Provide the required opening <tool_call> and stop at the first actual
 * closing tag, before sampling any second call. The 2048 variant accepts
 * the original R4 system message, but its required-prefix framing is NOT
 * byte-identical to R4. Default remains a BLOCKING proof. Tensor builds can
 * explicitly opt into bounded cooperative polling after ag_loop_init. */
#if QW_CONTEXT_MAX == 1024u
#define QW_AGENT_PROTOCOL_VERSION "candidate-v5-singlecall-ctx1024"
#elif QW_CONTEXT_MAX == 2048u
#define QW_AGENT_PROTOCOL_VERSION "r4-system-context2048-required-framing"
#else
#error "Agent protocol supports only QW_CONTEXT_MAX=1024 or 2048"
#endif
#define QW_AGENT_STRUCTURAL_PREFIX "<tool_call>\n"
#define QW_AGENT_GENERATION_RESERVE 128u
#ifndef QW_AGENT_PROMPT_MAX
#define QW_AGENT_PROMPT_MAX 4096u
#endif

typedef enum {
    QW_AGENT_STAGE_OK = 0, QW_AGENT_STAGE_BINDING, QW_AGENT_STAGE_PROFILE,
    QW_AGENT_STAGE_CONTEXT, QW_AGENT_STAGE_OUTPUT, QW_AGENT_STAGE_TOKENIZER,
    QW_AGENT_STAGE_PREFILL, QW_AGENT_STAGE_SAMPLE, QW_AGENT_STAGE_DECODE,
    QW_AGENT_STAGE_STOP, QW_AGENT_STAGE_NORMALIZE, QW_AGENT_STAGE_RECEIPT,
    QW_AGENT_STAGE_SEQUENCE, QW_AGENT_STAGE_NEEDS_RESET,
    QW_AGENT_STAGE_UNKNOWN_LEASE, QW_AGENT_STAGE_TIMEOUT,
    QW_AGENT_STAGE_CANCELLED, QW_AGENT_STAGE_CLOCK, QW_AGENT_STAGE_TOOL_TICK
} qw_agent_stage;

typedef enum {
    QW_AGENT_BOUNDARY_NONE = 0,
    QW_AGENT_BOUNDARY_MODEL_SAMPLED_IM_END,
    QW_AGENT_BOUNDARY_PROVIDED_AFTER_FIRST_CALL,
    QW_AGENT_BOUNDARY_INVALID_SHARED_TOKEN_TAIL
} qw_agent_boundary_origin;
const char *qw_agent_boundary_name(qw_agent_boundary_origin origin);

typedef uint64_t (*qw_agent_now_ms_fn)(void *user); /* real monotonic milliseconds */
typedef int (*qw_agent_cancel_fn)(void *user); /* nonzero means cancel */
typedef void (*qw_agent_token_fn)(void *user, uint32_t position,
    uint32_t token, float selected_logit, const char *piece,
    size_t piece_bytes, int is_stop);
/* Suffix excludes the provided "<tool_call>\n". Payload includes it.
 * stop_token is the pending chat control, NOT necessarily model-sampled;
 * inspect bridge->boundary_origin. on_token still reports actual samples only. */
typedef void (*qw_agent_text_fn)(void *user, unsigned turn,
    const char *actual_generated_suffix, size_t suffix_bytes,
    const char *protocol_payload, size_t payload_bytes,
    uint32_t stop_token, const char *normalized_json);
typedef void (*qw_agent_receipt_fn)(void *user, const char *wire_id,
    const char *receipt_json);

typedef struct {
    qw_model *model; /* borrowed: own arena/UNKNOWN lease remains caller-owned */
    const qw_tokenizer *tokenizer; /* caller must qt_open with verified SHA256 */
    ag_loop *loop; /* caller initializes with qw_agent_protocol_adapter */
    const char *canonical_system; /* selected context profile; borrowed */
    size_t system_bytes;
    void *tokenizer_workspace; size_t tokenizer_workspace_bytes;
    uint32_t *tokens; size_t token_capacity; /* >= QW_CONTEXT_MAX */
    char *prompt; size_t prompt_capacity; /* >= QW_AGENT_PROMPT_MAX preferred */
    /* Compatibility field name: stores PROVIDED prefix plus real generated
     * suffix (the full XML protocol payload), not purely generated text. */
    char *generated; size_t generated_capacity; /* >= QW_TOOL_TEXT_MAX + 1 */
    char *receipt; size_t receipt_capacity; /* >= AG_LOOP_RESPONSE_MAX + 1 */
    qw_agent_now_ms_fn now_ms; qw_agent_cancel_fn cancelled;
    void *clock_user;
    qw_agent_token_fn on_token; qw_agent_text_fn on_text;
    qw_agent_receipt_fn on_receipt; void *trace_user;
    qw_tool_normalizer normalizer;
    /* One PENDING edge after generation lets ag_loop_step reacquire a fresh
     * now_ms immediately before ag_execute (necessary for real timers). */
    char ready_response[AG_LOOP_RESPONSE_MAX + 1];
    size_t ready_bytes; unsigned ready;
    qw_agent_stage stage; int model_error; qw_norm_status norm_error;
    uint32_t expected_position, stop_token, canonical_im_end;
    qw_agent_boundary_origin boundary_origin;
    uint64_t last_ms;
    unsigned turn, awaiting_receipt, stop_pending, started;
    unsigned cooperative;
#ifdef QW_ENABLE_TENSOR_V2
    qw_tensor_task model_task; /* UNKNOWN retains this storage and model arena */
    unsigned task_active, cooperative_phase, generation_tokens;
    size_t prefill_count, prefill_index, generated_bytes;
    const float *cooperative_logits;
#endif
} qw_agent_protocol_backend;

/* Initialize only on a fresh qw_model at position 0. No reset or inference
 * occurs here (qw_get_status reads model state). Caller must initialize
 * ag_loop using adapter AFTER this call. task_key must be unique among
 * tasks sharing an ag_runtime. Returns 1 on success, 0 on failure;
 * on failure inspect bridge->stage and bridge->model_error. */
int qw_agent_protocol_init(qw_agent_protocol_backend *bridge, qw_model *model,
    const qw_tokenizer *tokenizer, ag_loop *loop, const char *canonical_system,
    size_t system_bytes, uint32_t task_key, void *tokenizer_workspace,
    size_t tokenizer_workspace_bytes, uint32_t *tokens, size_t token_capacity,
    char *prompt, size_t prompt_capacity, char *protocol_payload,
    size_t payload_capacity, char *receipt, size_t receipt_capacity,
    qw_agent_now_ms_fn now_ms, qw_agent_cancel_fn cancelled, void *clock_user);

/* Opt-in only, AFTER ag_loop_init and BEFORE submit. Requires a tensor-enabled
 * model; registers the loop cancel hook without changing ag_loop_backend ABI.
 * Non-tensor builds return 0. Default blocking behavior is unchanged. */
int qw_agent_protocol_enable_cooperative(qw_agent_protocol_backend *bridge);
/* Safe-boundary cleanup; never means DMA stop. UNKNOWN keeps model_task/arena.
 * May also be installed by an embedding caller as the loop cancel hook. */
void qw_agent_protocol_cancel(void *context, ag_loop_status reason);

/* Default poll blocks until a real stop or failure, ignoring work_quota. Do not
 * attach to a responsive GUI event loop. Deadline/cancel are checked
 * before and after the single prefill call and between decode tokens;
 * an in-flight qw_prefill cannot be interrupted by this adapter.
 * Cooperative mode performs at most ONE model work unit for positive quota,
 * returning PROGRESS after actual advancement. Zero quota does no work.
 * Unit latency is unmeasured until board testing; there is no 100ms guarantee.
 * All calls, including cancellation, must be serialized on the polling thread. */
ag_backend_poll_status qw_agent_protocol_poll(void *context,
    const ag_loop_input *input, uint32_t work_quota, char *response,
    size_t response_capacity, size_t *response_bytes);
ag_loop_backend qw_agent_protocol_adapter(qw_agent_protocol_backend *bridge);
#define AGENT_DIRECT_MAX_TOKENS 96u
int qw_agent_direct_chat(qw_agent_protocol_backend *,const char *);

/* One complete task, no per-turn model reset, no scripted-token fallback.
 * Return AG_LOOP_FINISHED or AG_LOOP_CLARIFY on success; all failures return
 * loop->status, with the more specific bridge->stage/model_error/norm_error.
 * On timeout/cancel inside a blocking model call, loop->status is
 * AG_LOOP_BACKEND_ERROR but bridge->stage remains TIMEOUT/CANCELLED. */
ag_loop_status qw_agent_protocol_run(qw_agent_protocol_backend *bridge,
    const char *task, uint32_t timeout_ms, unsigned max_turns);
const char *qw_agent_stage_name(qw_agent_stage stage);

#endif
