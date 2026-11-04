#include "qw_agent_protocol_backend.h"
#include <string.h>
#ifdef QW_ENABLE_TENSOR_V2
#include "../model/qw_tensor_v2.h"
#endif


static const char system_head[] = "<|im_start|>system\n";
static const char first_user[] = "<|im_end|>\n<|im_start|>user\n";
static const char assistant_head[] =
    "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"
    QW_AGENT_STRUCTURAL_PREFIX;
static const char tool_head[] = "\n<|im_start|>user\n<tool_response>\n";
static const char tool_tail[] =
    "\n</tool_response><|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"
    QW_AGENT_STRUCTURAL_PREFIX;

static int failure(qw_agent_protocol_backend *b, qw_agent_stage stage, int rc) {
    b->stage = rc == QW_UNKNOWN ? QW_AGENT_STAGE_UNKNOWN_LEASE :
               rc == QW_NEED_RESET ? QW_AGENT_STAGE_NEEDS_RESET :
               rc == QW_CONTEXT_FULL ? QW_AGENT_STAGE_CONTEXT : stage;
    b->model_error = rc;
    return 0;
}

static int append(char *dst, size_t cap, size_t *used,
                  const char *src, size_t bytes) {
    if (!src || !cap || bytes >= cap || *used > cap - bytes - 1u) return 0;
    memcpy(dst + *used, src, bytes);
    *used += bytes; dst[*used] = 0;
    return 1;
}
static int literal(char *dst, size_t cap, size_t *used, const char *src) {
    return append(dst, cap, used, src, strlen(src));
}

static int checkpoint(qw_agent_protocol_backend *b) {
    uint64_t now = b->now_ms(b->clock_user);
    if (now < b->last_ms) return failure(b, QW_AGENT_STAGE_CLOCK, 0);
    b->last_ms = now;
    if (b->cancelled && b->cancelled(b->clock_user))
        return failure(b, QW_AGENT_STAGE_CANCELLED, 0);
    if (now >= b->loop->deadline_ms)
        return failure(b, QW_AGENT_STAGE_TIMEOUT, 0);
    /* ag_loop_step invokes ag_execute with a fresh timestamp on the next
     * step; ticking here also keeps already-running timers truthful. */
    if (ag_tick(&b->loop->runtime, now) != AG_OK)
        return failure(b, QW_AGENT_STAGE_TOOL_TICK, 0);
    return 1;
}

static int model_status(qw_agent_protocol_backend *b, const float **logits) {
    qw_status s;
    int rc = qw_get_status(b->model, &s);
    if (rc) return failure(b, QW_AGENT_STAGE_BINDING, rc);
    if (s.lease_active || s.last_error == QW_UNKNOWN)
        return failure(b, QW_AGENT_STAGE_UNKNOWN_LEASE, QW_UNKNOWN);
    if (s.needs_reset) return failure(b, QW_AGENT_STAGE_NEEDS_RESET, s.last_error);
    if (s.position != b->expected_position)
        return failure(b, QW_AGENT_STAGE_SEQUENCE, 0);
    if (logits) {
        if (!s.logits || s.logits_count != QW_VOCAB)
            return failure(b, QW_AGENT_STAGE_PREFILL, s.last_error);
        *logits = s.logits;
    }
    return 1;
}

static int encode_prompt(qw_agent_protocol_backend *b, size_t n, size_t *count) {
    int rc = qt_encode(b->tokenizer, b->prompt, n, 1, b->tokens,
        b->token_capacity, count, b->tokenizer_workspace,
        b->tokenizer_workspace_bytes);
    if (rc) return failure(b, QW_AGENT_STAGE_TOKENIZER, rc);
    if (!*count || *count > QW_CONTEXT_MAX - QW_AGENT_GENERATION_RESERVE ||
        b->expected_position > QW_CONTEXT_MAX - QW_AGENT_GENERATION_RESERVE - *count)
        return failure(b, QW_AGENT_STAGE_CONTEXT, QW_CONTEXT_FULL);
    return 1;
}

static int do_prefill(qw_agent_protocol_backend *b, size_t n, const float **logits) {
    size_t processed = 0;
    if (!checkpoint(b)) return 0;
    int rc = qw_prefill(b->model, b->tokens, n, &processed);
    b->expected_position += (uint32_t)processed;
    if (rc) return failure(b, QW_AGENT_STAGE_PREFILL, rc);
    if (processed != n) return failure(b, QW_AGENT_STAGE_PREFILL, QW_IO);
    if (!checkpoint(b)) return 0;
    return model_status(b, logits);
}

static int build_first(qw_agent_protocol_backend *b, const char *task,
                       size_t *bytes) {
    size_t n = 0, task_bytes = 0;
    while (task_bytes <= AG_LOOP_TASK_MAX && task[task_bytes]) ++task_bytes;
    if (!task_bytes || task_bytes > AG_LOOP_TASK_MAX || strstr(task, "<|"))
        return failure(b, QW_AGENT_STAGE_PROFILE, 0);
    b->prompt[0] = 0;
    if (!literal(b->prompt, b->prompt_capacity, &n, system_head) ||
        !append(b->prompt, b->prompt_capacity, &n, b->canonical_system, b->system_bytes) ||
        !literal(b->prompt, b->prompt_capacity, &n, first_user) ||
        !append(b->prompt, b->prompt_capacity, &n, task, task_bytes) ||
        !literal(b->prompt, b->prompt_capacity, &n, assistant_head))
        return failure(b, QW_AGENT_STAGE_OUTPUT, QW_CAPACITY);
    *bytes = n;
    return 1;
}

static int build_receipt(qw_agent_protocol_backend *b,
                         const ag_loop_input *input, size_t *bytes) {
    ag_receipt receipt; size_t n = 0, text_bytes;
    if (!input->call_id || !input->receipt_text ||
        strcmp(input->call_id, b->normalizer.calls[b->normalizer.count - 1u].wire_id) ||
        input->receipt_code < AG_OK || input->receipt_code > AG_STORAGE)
        return failure(b, QW_AGENT_STAGE_SEQUENCE, 0);
    text_bytes = 0;
    while (text_bytes <= AG_RESULT_MAX && input->receipt_text[text_bytes]) ++text_bytes;
    if (text_bytes > AG_RESULT_MAX) return failure(b, QW_AGENT_STAGE_RECEIPT, 0);
    receipt.code = input->receipt_code;
    memcpy(receipt.result, input->receipt_text, text_bytes + 1u);
    b->norm_error = qw_tool_format_receipt(&b->normalizer, &b->loop->runtime,
        input->call_id, &receipt, b->receipt, b->receipt_capacity);
    if (b->norm_error != QW_NORM_OK)
        return failure(b, QW_AGENT_STAGE_RECEIPT, 0);
    if (b->on_receipt) b->on_receipt(b->trace_user, input->call_id, b->receipt);
    b->prompt[0] = 0;
    if (!literal(b->prompt, b->prompt_capacity, &n, tool_head) ||
        !literal(b->prompt, b->prompt_capacity, &n, b->receipt) ||
        !literal(b->prompt, b->prompt_capacity, &n, tool_tail))
        return failure(b, QW_AGENT_STAGE_OUTPUT, QW_CAPACITY);
    *bytes = n;
    return 1;
}

static int is_im_end_piece(uint32_t token, const char *piece, size_t bytes) {
    static const char im_end[] = "<|im_end|>";
    return qw_is_stop(token) && piece && bytes == sizeof(im_end) - 1u &&
           memcmp(piece, im_end, sizeof(im_end) - 1u) == 0;
}

static int resolve_im_end(qw_agent_protocol_backend *b) {
    static const char im_end[] = "<|im_end|>";
    size_t count = 0, piece_bytes = 0; const char *piece = 0;
    int rc = qt_encode(b->tokenizer, im_end, sizeof(im_end) - 1u, 1,
        b->tokens, b->token_capacity, &count, b->tokenizer_workspace,
        b->tokenizer_workspace_bytes);
    if (rc) return failure(b, QW_AGENT_STAGE_TOKENIZER, rc);
    if (count != 1u || qt_piece(b->tokenizer, b->tokens[0],
        &piece, &piece_bytes, 0) ||
        !is_im_end_piece(b->tokens[0], piece, piece_bytes))
        return failure(b, QW_AGENT_STAGE_STOP, QW_TOKEN);
    b->canonical_im_end = b->tokens[0];
    return 1;
}

static void report_payload(qw_agent_protocol_backend *b, unsigned turn,
                           const char *normalized) {
    if (!b->on_text) return;
    const size_t prefix_bytes = sizeof(QW_AGENT_STRUCTURAL_PREFIX) - 1u;
    const size_t payload_bytes = strlen(b->generated);
    const size_t offset = payload_bytes >= prefix_bytes ? prefix_bytes : payload_bytes;
    b->on_text(b->trace_user, turn, b->generated + offset,
        payload_bytes - offset, b->generated, payload_bytes,
        b->stop_token, normalized);
}

/* Detect FIRST closing tag across token pieces without deleting any bytes.
 * Whitespace sharing its final piece is retained. Any other tail is invalid. */
static int first_tool_close(const char *payload) {
    static const char close[] = "</tool_call>";
    const char *tail = strstr(payload, close);
    if (!tail) return 0;
    tail += sizeof(close) - 1u;
    while (*tail) {
        if (*tail != ' ' && *tail != '\t' && *tail != '\n' && *tail != '\r') return -1;
        ++tail;
    }
    return 1;
}

static int prepend_stop_once(qw_agent_protocol_backend *b, size_t *count) {
    const char *piece = 0; size_t piece_bytes = 0;
    if (!b->stop_pending ||
        qt_piece(b->tokenizer, b->stop_token, &piece, &piece_bytes, 0) ||
        !is_im_end_piece(b->stop_token, piece, piece_bytes))
        return failure(b, QW_AGENT_STAGE_STOP, QW_TOKEN);
    if (*count >= b->token_capacity ||
        *count > QW_CONTEXT_MAX - QW_AGENT_GENERATION_RESERVE ||
        b->expected_position >= QW_CONTEXT_MAX - *count - QW_AGENT_GENERATION_RESERVE)
        return failure(b, QW_AGENT_STAGE_CONTEXT, QW_CONTEXT_FULL);
    for (size_t i = *count; i > 0; --i) b->tokens[i] = b->tokens[i - 1u];
    b->tokens[0] = b->stop_token; ++*count;
    return 1;
}

static int generate(qw_agent_protocol_backend *b, const float *logits) {
    size_t length = 0;
    b->generated[0] = 0;
    b->stop_token = 0; b->stop_pending = 0;
    b->boundary_origin = QW_AGENT_BOUNDARY_NONE;
    /* This exact prefix is already PREFILLED, not sampled. Keep it in the
     * protocol payload so the strict normalizer sees actual canonical XML. */
    if (!literal(b->generated, b->generated_capacity, &length,
                 QW_AGENT_STRUCTURAL_PREFIX))
        return failure(b, QW_AGENT_STAGE_OUTPUT, QW_CAPACITY);
    for (unsigned i = 0; i < QW_AGENT_GENERATION_RESERVE; ++i) {
        uint32_t token = 0, seed = 1, position = b->expected_position;
        const char *piece; size_t piece_bytes; unsigned flags = 0;
        if (!checkpoint(b)) return 0;
        int rc = qw_sample(logits, QW_VOCAB, 0.0f, 1u, &seed, &token);
        if (rc) return failure(b, QW_AGENT_STAGE_SAMPLE, rc);
        if ((rc = qt_piece(b->tokenizer, token, &piece, &piece_bytes, &flags)))
            return failure(b, QW_AGENT_STAGE_TOKENIZER, rc);
        int stop = qw_is_stop(token);
        if (b->on_token) b->on_token(b->trace_user, position, token,
            logits[token], piece, piece_bytes, stop);
        if (stop) {
            if (!is_im_end_piece(token, piece, piece_bytes))
                return failure(b, QW_AGENT_STAGE_STOP, QW_TOKEN);
            b->stop_token = token; b->stop_pending = 1;
            b->boundary_origin = QW_AGENT_BOUNDARY_MODEL_SAMPLED_IM_END;
            return checkpoint(b);
        }
        if (memchr(piece, 0, piece_bytes) ||
            !append(b->generated, b->generated_capacity, &length, piece, piece_bytes) ||
            length > QW_TOOL_TEXT_MAX)
            return failure(b, QW_AGENT_STAGE_OUTPUT, QW_CAPACITY);
        if (position >= QW_CONTEXT_MAX)
            return failure(b, QW_AGENT_STAGE_CONTEXT, QW_CONTEXT_FULL);
        rc = qw_decode(b->model, token, &logits);
        if (rc) return failure(b, QW_AGENT_STAGE_DECODE, rc);
        ++b->expected_position;
        if (!checkpoint(b) || !model_status(b, &logits)) return 0;
        int boundary = first_tool_close(b->generated);
        if (boundary < 0) {
            b->boundary_origin = QW_AGENT_BOUNDARY_INVALID_SHARED_TOKEN_TAIL;
            report_payload(b, b->turn + 1u, "");
            return failure(b, QW_AGENT_STAGE_STOP, QW_TOKEN);
        }
        if (boundary > 0) {
            /* Closing-tag token is already consumed. No later token is
             * sampled. Chat im_end is a PROVIDED pending framing control. */
            b->stop_token = b->canonical_im_end; b->stop_pending = 1;
            b->boundary_origin = QW_AGENT_BOUNDARY_PROVIDED_AFTER_FIRST_CALL;
            return 1;
        }
    }
    return failure(b, QW_AGENT_STAGE_STOP, QW_CONTEXT_FULL);
}

int qw_agent_protocol_init(qw_agent_protocol_backend *b, qw_model *model,
    const qw_tokenizer *tokenizer, ag_loop *loop, const char *canonical_system,
    size_t system_bytes, uint32_t task_key, void *workspace, size_t workspace_bytes,
    uint32_t *tokens, size_t token_capacity, char *prompt, size_t prompt_capacity,
    char *protocol_payload, size_t payload_capacity, char *receipt,
    size_t receipt_capacity, qw_agent_now_ms_fn now_ms,
    qw_agent_cancel_fn cancelled, void *clock_user) {
    if (!b) return 0;
    memset(b, 0, sizeof(*b)); b->stage = QW_AGENT_STAGE_BINDING;
    if (!model || !tokenizer || !tokenizer->data || !loop ||
        !canonical_system || !system_bytes ||
        system_bytes > QW_AGENT_PROMPT_MAX - AG_LOOP_TASK_MAX - 128u ||
        !task_key || !workspace || workspace_bytes < qt_workspace_bytes(QW_AGENT_PROMPT_MAX) ||
        !tokens || token_capacity < QW_CONTEXT_MAX ||
        !prompt || prompt_capacity < QW_AGENT_PROMPT_MAX ||
        !protocol_payload || payload_capacity < QW_TOOL_TEXT_MAX + 1u ||
        !receipt || receipt_capacity < AG_LOOP_RESPONSE_MAX + 1u || !now_ms) return 0;
    qw_status s; int rc = qw_get_status(model, &s);
    if (rc) return failure(b, QW_AGENT_STAGE_BINDING, rc);
    if (s.lease_active || s.last_error == QW_UNKNOWN)
        return failure(b, QW_AGENT_STAGE_UNKNOWN_LEASE, QW_UNKNOWN);
    if (s.position || s.needs_reset)
        return failure(b, QW_AGENT_STAGE_NEEDS_RESET, s.last_error);
    b->model = model; b->tokenizer = tokenizer; b->loop = loop;
    b->canonical_system = canonical_system; b->system_bytes = system_bytes;
    b->tokenizer_workspace = workspace; b->tokenizer_workspace_bytes = workspace_bytes;
    b->tokens = tokens; b->token_capacity = token_capacity;
    b->prompt = prompt; b->prompt_capacity = prompt_capacity;
    b->generated = protocol_payload; b->generated_capacity = payload_capacity;
    b->receipt = receipt; b->receipt_capacity = receipt_capacity;
    b->now_ms = now_ms; b->cancelled = cancelled; b->clock_user = clock_user;
    if (!resolve_im_end(b)) return 0;
    qw_tool_normalizer_init(&b->normalizer, task_key);
    b->stage = QW_AGENT_STAGE_OK;
    return 1;
}

static ag_backend_poll_status finish_generation(qw_agent_protocol_backend *b) {
    ++b->turn;
    const size_t prefix_bytes = sizeof(QW_AGENT_STRUCTURAL_PREFIX) - 1u;
    const size_t payload_bytes = strlen(b->generated);
    if (payload_bytes < prefix_bytes) {
        failure(b, QW_AGENT_STAGE_SEQUENCE, 0);
        return AG_BACKEND_FAILED;
    }
    const char *suffix = b->generated + prefix_bytes;
    const size_t suffix_bytes = payload_bytes - prefix_bytes;
    b->norm_error = qw_tool_normalize(&b->normalizer, &b->loop->runtime,
        b->generated, payload_bytes, 0, b->ready_response,
        sizeof(b->ready_response));
    if (b->norm_error != QW_NORM_OK) {
        if (b->on_text) b->on_text(b->trace_user, b->turn, suffix,
            suffix_bytes, b->generated, payload_bytes, b->stop_token, "");
        failure(b, QW_AGENT_STAGE_NORMALIZE, 0);
        return AG_BACKEND_FAILED;
    }
    b->ready_bytes = strlen(b->ready_response);
    if (b->on_text) b->on_text(b->trace_user, b->turn, suffix,
        suffix_bytes, b->generated, payload_bytes, b->stop_token,
        b->ready_response);
    b->awaiting_receipt = strncmp(b->ready_response, "{\"type\":\"tool\"", 14u) == 0;
    b->ready = 1;
    /* Returning PENDING once is intentional: next ag_loop_step captures a fresh
     * monotonic now_ms just before ag_execute (not stale pre-inference time). */
    return AG_BACKEND_PENDING;
}

/* Inserted into qw_agent_protocol_backend.c by the implementation helper. */
void qw_agent_protocol_cancel(void *context, ag_loop_status reason) {
    qw_agent_protocol_backend *b = context;
    if (!b) return;
    if (b->stage == QW_AGENT_STAGE_OK)
        b->stage = reason == AG_LOOP_TIMEOUT ? QW_AGENT_STAGE_TIMEOUT :
                   reason == AG_LOOP_CANCELLED ? QW_AGENT_STAGE_CANCELLED :
                   QW_AGENT_STAGE_SEQUENCE;
#ifdef QW_ENABLE_TENSOR_V2
    if (b->cooperative && b->task_active) {
        qw_status status;
        int rc = qw_get_status(b->model, &status);
        if (rc == QW_UNKNOWN || (!rc &&
            (status.lease_active || status.last_error == QW_UNKNOWN)) ||
            b->model_task.state == QW_TENSOR_TASK_UNKNOWN) {
            (void)failure(b, QW_AGENT_STAGE_UNKNOWN_LEASE, QW_UNKNOWN);
            return; /* UNKNOWN retains task and caller arena; no discard. */
        }
        if (rc) { (void)failure(b, QW_AGENT_STAGE_BINDING, rc); return; }
        rc = qw_tensor_task_discard(&b->model_task);
        if (rc) { (void)failure(b, QW_AGENT_STAGE_NEEDS_RESET, rc); return; }
        b->task_active = 0;
    }
#endif
}

int qw_agent_protocol_enable_cooperative(qw_agent_protocol_backend *b) {
#ifdef QW_ENABLE_TENSOR_V2
    qw_tensor_status status;
    if (!b || b->stage != QW_AGENT_STAGE_OK || b->started ||
        !b->loop || b->loop->status != AG_LOOP_IDLE ||
        b->loop->backend.poll != qw_agent_protocol_poll ||
        b->loop->backend.context != b) return 0;
    int rc = qw_tensor_get_status(b->model, &status);
    if (rc || !status.enabled || status.unknown) return 0;
    b->cooperative = 1;
    ag_loop_set_cancel(b->loop, qw_agent_protocol_cancel, b);
    return 1;
#else
    (void)b;
    return 0;
#endif
}

#ifdef QW_ENABLE_TENSOR_V2
enum { COOP_PREPARE = 0, COOP_PREFILL, COOP_SAMPLE, COOP_DECODE, COOP_FINISH };

static int cooperative_prepare(qw_agent_protocol_backend *b,
                               const ag_loop_input *input) {
    size_t bytes = 0, count = 0; unsigned boundary = 0;
    if (!b->started && input->kind == AG_LOOP_INPUT_TASK) {
        if (!input->task) return failure(b, QW_AGENT_STAGE_PROFILE, 0);
        if (!build_first(b, input->task, &bytes)) return 0;
        b->started = 1;
    } else if (b->started && b->awaiting_receipt && b->stop_pending &&
               input->kind == AG_LOOP_INPUT_TOOL_RECEIPT && b->normalizer.count) {
        if (!build_receipt(b, input, &bytes)) return 0;
        boundary = 1; b->awaiting_receipt = 0;
    } else return failure(b, QW_AGENT_STAGE_SEQUENCE, 0);
    if (!encode_prompt(b, bytes, &count) ||
        (boundary && !prepend_stop_once(b, &count))) return 0;
    b->prefill_count = count; b->prefill_index = 0;
    b->generated_bytes = 0; b->generated[0] = 0;
    b->generation_tokens = 0; b->boundary_origin = QW_AGENT_BOUNDARY_NONE;
    if (!literal(b->generated, b->generated_capacity, &b->generated_bytes,
                 QW_AGENT_STRUCTURAL_PREFIX))
        return failure(b, QW_AGENT_STAGE_OUTPUT, QW_CAPACITY);
    b->cooperative_phase = COOP_PREFILL;
    return 1;
}

static int cooperative_sample(qw_agent_protocol_backend *b) {
    uint32_t seed = 1, token = 0;
    const char *piece; size_t bytes; unsigned flags = 0;
    if (b->generation_tokens >= QW_AGENT_GENERATION_RESERVE)
        return failure(b, QW_AGENT_STAGE_STOP, QW_CONTEXT_FULL);
    int rc = qw_sample(b->cooperative_logits, QW_VOCAB, 0.0f, 1u, &seed, &token);
    if (rc) return failure(b, QW_AGENT_STAGE_SAMPLE, rc);
    if ((rc = qt_piece(b->tokenizer, token, &piece, &bytes, &flags)))
        return failure(b, QW_AGENT_STAGE_TOKENIZER, rc);
    ++b->generation_tokens;
    int stop = qw_is_stop(token);
    if (b->on_token) b->on_token(b->trace_user, b->expected_position, token,
        b->cooperative_logits[token], piece, bytes, stop);
    if (stop) {
        if (!is_im_end_piece(token, piece, bytes))
            return failure(b, QW_AGENT_STAGE_STOP, QW_TOKEN);
        b->stop_token = token; b->stop_pending = 1;
        b->boundary_origin = QW_AGENT_BOUNDARY_MODEL_SAMPLED_IM_END;
        b->cooperative_phase = COOP_FINISH;
        return 1;
    }
    if (memchr(piece, 0, bytes) ||
        !append(b->generated, b->generated_capacity, &b->generated_bytes, piece, bytes) ||
        b->generated_bytes > QW_TOOL_TEXT_MAX)
        return failure(b, QW_AGENT_STAGE_OUTPUT, QW_CAPACITY);
    if (b->expected_position >= QW_CONTEXT_MAX)
        return failure(b, QW_AGENT_STAGE_CONTEXT, QW_CONTEXT_FULL);
    memset(&b->model_task, 0, sizeof(b->model_task));
    rc = qw_tensor_task_begin(&b->model_task, b->model, token, 1);
    if (rc) return failure(b, QW_AGENT_STAGE_DECODE, rc);
    b->task_active = 1; b->cooperative_phase = COOP_DECODE;
    return 1;
}

static ag_backend_poll_status cooperative_poll(qw_agent_protocol_backend *b,
    const ag_loop_input *input, uint32_t quota) {
    if (!quota) return AG_BACKEND_PENDING;
    if (!b->task_active && !model_status(b, 0)) return AG_BACKEND_FAILED;
    if (b->cooperative_phase == COOP_PREPARE && !cooperative_prepare(b, input))
        return AG_BACKEND_FAILED;
    if (b->cooperative_phase == COOP_SAMPLE && !cooperative_sample(b))
        return AG_BACKEND_FAILED;
    if (b->cooperative_phase == COOP_FINISH) {
        ag_backend_poll_status result = finish_generation(b);
        if (result != AG_BACKEND_FAILED) b->cooperative_phase = COOP_PREPARE;
        return result;
    }
    int prefill = b->cooperative_phase == COOP_PREFILL;
    if (!b->task_active) {
        memset(&b->model_task, 0, sizeof(b->model_task));
        int rc = qw_tensor_task_begin(&b->model_task, b->model,
            b->tokens[b->prefill_index], b->prefill_index + 1u == b->prefill_count);
        if (rc) { failure(b, QW_AGENT_STAGE_PREFILL, rc); return AG_BACKEND_FAILED; }
        b->task_active = 1;
    }
    uint32_t before = b->model_task.work_units;
    const float *logits = 0;
    /* Yield after one primitive: <= quota, without a measured wall-time claim. */
    int rc = qw_tensor_task_advance(&b->model_task, 1u, &logits);
    if (rc != 0 && rc != QW_PENDING) {
        failure(b, prefill ? QW_AGENT_STAGE_PREFILL : QW_AGENT_STAGE_DECODE, rc);
        return AG_BACKEND_FAILED;
    }
    if (!rc) {
        b->task_active = 0;
        ++b->expected_position;
    }
    if (!checkpoint(b)) return AG_BACKEND_FAILED;
    if (!rc) {
        if (!model_status(b, 0)) return AG_BACKEND_FAILED;
        if (prefill) {
            if (b->prefill_index == 0) b->stop_pending = 0;
            if (++b->prefill_index == b->prefill_count) {
                if (!logits) { failure(b, QW_AGENT_STAGE_PREFILL, QW_IO); return AG_BACKEND_FAILED; }
                b->cooperative_logits = logits; b->cooperative_phase = COOP_SAMPLE;
            }
        } else {
            if (!logits) { failure(b, QW_AGENT_STAGE_DECODE, QW_IO); return AG_BACKEND_FAILED; }
            b->cooperative_logits = logits;
            int boundary = first_tool_close(b->generated);
            if (boundary < 0) {
                b->boundary_origin = QW_AGENT_BOUNDARY_INVALID_SHARED_TOKEN_TAIL;
                report_payload(b, b->turn + 1u, "");
                failure(b, QW_AGENT_STAGE_STOP, QW_TOKEN); return AG_BACKEND_FAILED;
            }
            if (boundary) {
                b->stop_token = b->canonical_im_end; b->stop_pending = 1;
                b->boundary_origin = QW_AGENT_BOUNDARY_PROVIDED_AFTER_FIRST_CALL;
            }
            b->cooperative_phase = boundary ? COOP_FINISH : COOP_SAMPLE;
        }
    }
    return b->model_task.work_units != before ? AG_BACKEND_PROGRESS : AG_BACKEND_PENDING;
}
#endif

ag_backend_poll_status qw_agent_protocol_poll(void *context,
    const ag_loop_input *input, uint32_t work_quota, char *response,
    size_t response_capacity, size_t *response_bytes) {
    (void)work_quota; /* Default remains the original blocking proof. */
    qw_agent_protocol_backend *b = context;
    if (response_bytes) *response_bytes = 0;
    if (response && response_capacity) response[0] = 0;
    if (!b || !response_bytes || !response || !response_capacity ||
        !input || b->stage != QW_AGENT_STAGE_OK) return AG_BACKEND_FAILED;
    if (!checkpoint(b)) {
        qw_agent_protocol_cancel(b, AG_LOOP_BACKEND_ERROR);
        return AG_BACKEND_FAILED;
    }
    if (!b->cooperative && !model_status(b, 0)) return AG_BACKEND_FAILED;
    if (b->ready) {
        if (b->ready_bytes > response_capacity ||
            b->ready_bytes >= sizeof(b->ready_response)) {
            failure(b, QW_AGENT_STAGE_OUTPUT, QW_CAPACITY);
            return AG_BACKEND_FAILED;
        }
        memcpy(response, b->ready_response, b->ready_bytes + 1u);
        *response_bytes = b->ready_bytes; b->ready = 0;
        return AG_BACKEND_READY;
    }
#ifdef QW_ENABLE_TENSOR_V2
    if (b->cooperative) {
        ag_backend_poll_status result = cooperative_poll(b, input, work_quota);
        if (result == AG_BACKEND_FAILED) qw_agent_protocol_cancel(b, AG_LOOP_BACKEND_ERROR);
        return result;
    }
#endif
    size_t bytes = 0, token_count = 0; const float *logits = 0;
    unsigned with_boundary = 0;
    if (!b->started && input->kind == AG_LOOP_INPUT_TASK) {
        if (!input->task) {
            failure(b, QW_AGENT_STAGE_PROFILE, 0);
            return AG_BACKEND_FAILED;
        }
        if (!build_first(b, input->task, &bytes)) return AG_BACKEND_FAILED;
        b->started = 1;
    } else if (b->started && b->awaiting_receipt && b->stop_pending &&
               input->kind == AG_LOOP_INPUT_TOOL_RECEIPT &&
               b->normalizer.count) {
        if (!build_receipt(b, input, &bytes)) return AG_BACKEND_FAILED;
        /* Pending im_end is either provided after the first closing tag or
         * sampled-but-unfed. Batch it ONCE with the real receipt framing. */
        with_boundary = 1;
        b->awaiting_receipt = 0;
    } else {
        failure(b, QW_AGENT_STAGE_SEQUENCE, 0);
        return AG_BACKEND_FAILED;
    }
    if (!encode_prompt(b, bytes, &token_count) ||
        (with_boundary && !prepend_stop_once(b, &token_count)) ||
        !do_prefill(b, token_count, &logits)) return AG_BACKEND_FAILED;
    if (with_boundary) b->stop_pending = 0;
    if (!generate(b, logits)) return AG_BACKEND_FAILED;
    return finish_generation(b);
}

/* Direct chat: no tool schema, no agent loop. The prompt is one user turn, so
 * prefill is a handful of tokens instead of the whole tool profile. */
int qw_agent_direct_chat(qw_agent_protocol_backend *b,const char *text){
    static const char sys[]="<|im_start|>system\nAnswer in English, one short sentence.\n<|im_end|>\n";
    static const char head[]="<|im_start|>user\n";
    static const char tail[]="<|im_end|>\n<|im_start|>assistant\n";
    if(!b||!text||!b->model||!b->tokenizer)return QW_ARGUMENT;
    size_t tb=0;while(text[tb]&&tb<AG_LOOP_TASK_MAX)++tb;
    if(!tb||tb>AG_LOOP_TASK_MAX)return QW_ARGUMENT;
    size_t n=0;
    b->prompt[0]=0;
    if(!literal(b->prompt,b->prompt_capacity,&n,sys)||
       !literal(b->prompt,b->prompt_capacity,&n,head)||
       !append(b->prompt,b->prompt_capacity,&n,text,tb)||
       !literal(b->prompt,b->prompt_capacity,&n,tail))return QW_CAPACITY;
    size_t count=0;
    if(qt_encode(b->tokenizer,b->prompt,n,1,b->tokens,b->token_capacity,&count,
        b->tokenizer_workspace,b->tokenizer_workspace_bytes))return QW_TOKEN;
    if(!count)return QW_TOKEN;
    size_t processed=0;
    int prc=qw_prefill(b->model,b->tokens,count,&processed);
    if(prc)return prc;                      /* surface the real code, never mask it */
    if(processed!=count)return QW_CONTEXT_FULL;
    qw_status st;
    int src=qw_get_status(b->model,&st);
    if(src)return src;
    if(!st.logits)return QW_NEED_RESET;
    const float *logits=st.logits;uint32_t token=0,seed=1;
    for(unsigned step=0;step<AGENT_DIRECT_MAX_TOKENS;step++){
        if(qw_sample(logits,QW_VOCAB,0,1,&seed,&token))return QW_NUMERIC;
        if(qw_is_stop(token))break;
        const char *piece=0;size_t bytes=0;unsigned flags=0;
        if(qt_piece(b->tokenizer,token,&piece,&bytes,&flags))return QW_TOKEN;
        if(piece&&bytes&&b->on_token)b->on_token(b->trace_user,step,token,0.0f,piece,bytes,0);
        if(qw_decode(b->model,token,&logits)||!logits)return QW_IO;
    }
    return 0;
}
ag_loop_backend qw_agent_protocol_adapter(qw_agent_protocol_backend *bridge) {
    ag_loop_backend backend = {qw_agent_protocol_poll, bridge};
    return backend;
}

ag_loop_status qw_agent_protocol_run(qw_agent_protocol_backend *b,
    const char *task, uint32_t timeout_ms, unsigned max_turns) {
    if (!b || b->stage != QW_AGENT_STAGE_OK || !b->loop || !task || b->started ||
        b->loop->status != AG_LOOP_IDLE ||
        b->loop->backend.poll != qw_agent_protocol_poll ||
        b->loop->backend.context != b) {
        if (b) failure(b, QW_AGENT_STAGE_SEQUENCE, 0);
        return AG_LOOP_PROTOCOL_ERROR;
    }
    b->last_ms = b->now_ms(b->clock_user);
    ag_loop_status state = ag_loop_submit(b->loop, task, b->last_ms,
        timeout_ms, max_turns);
    if (state != AG_LOOP_RUNNING) {
        failure(b, QW_AGENT_STAGE_BINDING, 0);
        return state;
    }
    do {
        uint64_t now = b->now_ms(b->clock_user);
        if (now < b->last_ms) {
            failure(b, QW_AGENT_STAGE_CLOCK, 0);
            ag_loop_cancel(b->loop); break;
        }
        if (b->cancelled && b->cancelled(b->clock_user)) {
            failure(b, QW_AGENT_STAGE_CANCELLED, 0);
            ag_loop_cancel(b->loop); break;
        }
        if (now >= b->loop->deadline_ms) {
            failure(b, QW_AGENT_STAGE_TIMEOUT, 0);
            state = ag_loop_step(b->loop, now); break;
        }
        b->last_ms = now;
        state = ag_loop_step(b->loop, now);
    } while (state == AG_LOOP_RUNNING);
    return b->loop->status;
}

const char *qw_agent_boundary_name(qw_agent_boundary_origin origin) {
    static const char *const names[] = {
        "none", "model_sampled_im_end", "provided_after_first_tool_call",
        "invalid_nonwhitespace_after_close_in_shared_token"
    };
    return (unsigned)origin < sizeof(names) / sizeof(names[0]) ? names[origin] : "invalid";
}

const char *qw_agent_stage_name(qw_agent_stage s) {
    static const char *const names[] = {
        "ok", "binding", "profile", "context", "output",
        "tokenizer", "prefill", "sample", "decode", "stop",
        "normalize", "receipt", "sequence", "needs_reset",
        "unknown_lease", "timeout", "cancelled", "clock", "tool_tick"
    };
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "invalid";
}
