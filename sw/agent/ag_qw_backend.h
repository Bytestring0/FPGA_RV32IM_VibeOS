#ifndef AG_QW_BACKEND_H
#define AG_QW_BACKEND_H

#include "ag_loop.h"
#include "../model/qw_runtime.h"
#include "../model/qw_tokenizer.h"

/* Caller retains TOKEN.BIN, prefix text/IDs and qt workspace throughout the session.
 * Each complete prompt version is pinned by system_key.template_sha256; IDs
 * must match qt_encode(parse_special=1), not a second hand-written template.
 * Model is caller-owned and remains leased on any UNKNOWN/cancel failure.
 * A software-reference qw_model is required; asynchronous hardware is unsupported. */
#define AG_QW_R4_TOKENS 1584u
#define AG_QW_PREFIX_UTF8_MAX 16384u
#define AG_QW_RAW_MAX 2048u
#define AG_QW_PROMPT_MAX 2048u

typedef enum {
    AG_QW_ERR_NONE, AG_QW_ERR_ARGUMENT, AG_QW_ERR_PREFIX, AG_QW_ERR_TOKENIZER,
    AG_QW_ERR_CONTEXT, AG_QW_ERR_MODEL, AG_QW_ERR_PROTOCOL, AG_QW_ERR_UTF8,
    AG_QW_ERR_OUTPUT, AG_QW_ERR_CANCELLED, AG_QW_ERR_LEASE
} ag_qw_error;

typedef struct {
    qw_model *model;
    const qw_tokenizer *tokenizer;
    const char *prefix_utf8;
    size_t prefix_bytes;
    const uint32_t *prefix_ids;
    size_t prefix_count;
    void *tokenizer_workspace;
    size_t tokenizer_workspace_bytes;
    uint32_t cpu_budget_ms; /* zero uses only the per-poll work quota */
    /* Caller reserves qw_checkpoint_bytes(model,prefix_count) separately from arena.
     * The key must bind pinned tokenizer/template/schema SHA256 and exact IDs.
     * Set checkpoint_candidate only for a persisted image of known origin; a
     * fresh buffer must use zero. Model + image remain live until quiescent. */
    const qw_checkpoint_key *system_key;
    void *checkpoint_image;
    size_t checkpoint_capacity;
    unsigned checkpoint_candidate;
} ag_qw_config;

typedef struct {
    qw_model *model;
    const qw_tokenizer *tokenizer;
    void *workspace;
    size_t workspace_bytes, used, prefix_count, in_flight, raw_len, response_len;
    uint32_t ids[QW_CONTEXT_MAX];
    char raw[AG_QW_RAW_MAX + 1];
    char response[AG_LOOP_RESPONSE_MAX + 1];
    char task[AG_LOOP_TASK_MAX + 1];
    char receipt_text[AG_RESULT_MAX + 1];
    char pending_call_id[AG_ID_MAX + 1];
    char receipt_call_id[AG_ID_MAX + 1];
    char last_tool_name[32];
    uint64_t generation;
    unsigned turn, kind, receipt_code, phase, operation, stop_pending;
    unsigned has_result, last_was_tool, utf8_left, utf8_min, utf8_max;
    uint32_t cpu_budget_ms;
    const qw_checkpoint_key *system_key;
    void *checkpoint_image;
    size_t checkpoint_capacity,checkpoint_bytes;
    unsigned checkpoint_ready,prefix_hit,prefix_builds;
    qw_checkpoint_job checkpoint_job;
    uint32_t last_generated_token,last_piece_flags,generated_tokens;
    const float *sampling_logits;
    uint32_t sampling_at,sampling_best;
    float sampling_max;
    int model_error;
    ag_qw_error error;
#ifdef AG_QW_BACKEND_TESTING
    struct ag_qw_mock_ops const *mock;
    void *mock_context;
#endif
} ag_qw_backend;

int ag_qw_backend_init(ag_qw_backend *b, const ag_qw_config *config);
ag_backend_poll_status ag_qw_backend_poll_v2(
    void *context, const ag_loop_input *input, uint32_t work_quota,
    char *response, size_t response_capacity, size_t *response_bytes,
    uint64_t *result_generation, unsigned *result_turn);
/* Call after ag_loop_cancel or timeout; caller then calls ag_loop_ack_quiescent
 * only if this returns 1. A failed cancellation never releases the lease. */
int ag_qw_backend_abort(ag_qw_backend *b, uint64_t generation);
int ag_qw_backend_quiescent(const ag_qw_backend *b);

#ifdef AG_QW_BACKEND_TESTING
/* Unit-only injection: bypasses R4/model verification; cannot be linked into
 * production without AG_QW_BACKEND_TESTING. Scripted sources are NOT evidence
 * that model weights generated any tokens. */
typedef struct ag_qw_mock_ops {
    int (*encode)(void *, const char *, size_t, uint32_t *, size_t, size_t *);
    int (*piece)(void *, uint32_t, const char **, size_t *, unsigned *);
    int (*start)(void *, const uint32_t *, size_t, int);
    int (*poll)(void *, uint32_t, uint32_t, size_t *, const float **);
    int (*sample)(void *, const float *, uint32_t *);
    int (*status)(void *, qw_status *);
    int (*reset)(void *);
    int (*cancel)(void *);
} ag_qw_mock_ops;
int ag_qw_backend_init_test(ag_qw_backend *, const ag_qw_mock_ops *, void *,
                            const uint32_t *, size_t);
#endif
#endif
