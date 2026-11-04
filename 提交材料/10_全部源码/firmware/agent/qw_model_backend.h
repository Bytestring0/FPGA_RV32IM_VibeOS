#ifndef AG_QW_MODEL_BACKEND_H
#define AG_QW_MODEL_BACKEND_H

#include "ag_loop.h"
#include "../model/qw_runtime.h"
#include "../model/qw_tokenizer.h"

/* Deliberately fail-closed integration boundary, NOT a model implementation.
 * See docs/qw_model_backend.md before enabling a model-backed Agent loop.
 * Neither the qw_model arena nor TOKEN.BIN is owned by this bridge. */
#define AG_QW_BACKEND_CONTEXT_MAX QW_CONTEXT_MAX
#if QW_CONTEXT_MAX != 1024u
#error "Re-evaluate Agent prompt and generation budgets for a new model context"
#endif

typedef enum {
    AG_QW_BRIDGE_INVALID_BINDING = 0,
    AG_QW_BRIDGE_CANONICAL_FORMAT_UNSUPPORTED,
    AG_QW_BRIDGE_UNKNOWN_LEASE,
    AG_QW_BRIDGE_NEEDS_RESET,
    AG_QW_BRIDGE_CONTEXT_CAPACITY,
    AG_QW_BRIDGE_OUTPUT_CAPACITY,
    AG_QW_BRIDGE_MODEL_ERROR
} ag_qw_bridge_reason;

typedef struct {
    qw_model *model;                 /* borrowed: do not free/close here */
    const qw_tokenizer *tokenizer;   /* borrowed: caller must have called qt_open */
    ag_qw_bridge_reason reason;
    int model_error;
} ag_qw_model_backend;

/* init binds without opening or touching hardware. start always fails until
 * the canonical template/tool protocol is specified and implemented. */
void ag_qw_model_backend_init(ag_qw_model_backend *bridge, qw_model *model,
                              const qw_tokenizer *tokenizer);
int ag_qw_model_backend_start(ag_qw_model_backend *bridge);

/* Explicit model reset only; QW_UNKNOWN retains the borrowed model and its
 * arena lease. Successful reset does NOT enable the unsupported backend. */
int ag_qw_model_backend_reset(ag_qw_model_backend *bridge);

typedef enum {
    AG_QW_GENERATE_DONE = 0,
    AG_QW_GENERATE_TOKEN_LIMIT = 1,
    AG_QW_GENERATE_OUTPUT_FULL = 2,
    AG_QW_GENERATE_FAILED = -1
} ag_qw_generate_status;

/* Called once per complete tokenizer piece, in generated order. Bytes need not
 * individually be UTF-8 complete; concatenated pieces form the decoded stream. */
typedef void (*ag_qw_piece_fn)(void *context, uint32_t token,
                               const char *bytes, size_t byte_count);

/* Generate from caller-rendered canonical Qwen prompt bytes. The prompt is
 * encoded with parse_special=1, then actual qw logits are sampled greedily.
 * output is always NUL-terminated when capacity>0 and receives only complete
 * pieces; token_scratch must hold at least QW_CONTEXT_MAX token IDs. */
ag_qw_generate_status ag_qw_model_backend_generate(
    ag_qw_model_backend *bridge, const char *canonical_prompt,
    size_t prompt_bytes, void *tokenizer_workspace,
    size_t tokenizer_workspace_bytes, uint32_t *token_scratch,
    size_t token_scratch_capacity, uint32_t max_new_tokens,
    char *output, size_t output_capacity, size_t *output_bytes,
    ag_qw_piece_fn on_piece, void *piece_context);

/* poll is nonblocking and ALWAYS returns AG_BACKEND_FAILED, response_bytes=0.
 * It never fabricates a JSON response, decodes tokens, or runs a fallback. */
ag_backend_poll_status ag_qw_model_backend_poll(
    void *context, const ag_loop_input *input, uint32_t work_quota,
    char *response, size_t response_capacity, size_t *response_bytes);
ag_loop_backend ag_qw_model_backend_adapter(ag_qw_model_backend *bridge);
const char *ag_qw_model_backend_reason(const ag_qw_model_backend *bridge);

#endif
