#ifndef BOARD_AGENT_LOOP_H
#define BOARD_AGENT_LOOP_H

#include "ag_agent.h"

#define AG_LOOP_API_VERSION 2u
#define AG_LOOP_TASK_MAX 512u
#define AG_LOOP_RESPONSE_MAX 768u
#define AG_LOOP_OUTPUT_MAX AG_RESULT_MAX
#define AG_LOOP_BACKEND_POLLS_PER_TURN 128u
#define AG_LOOP_MAX_TURNS 16u

typedef enum {
    AG_LOOP_IDLE = 0,
    AG_LOOP_RUNNING,
    AG_LOOP_FINISHED,
    AG_LOOP_CLARIFY,
    AG_LOOP_PROTOCOL_ERROR,
    AG_LOOP_BACKEND_ERROR,
    AG_LOOP_RESPONSE_TOO_LONG,
    AG_LOOP_MAX_TURNS_REACHED,
    AG_LOOP_TIMEOUT,
    AG_LOOP_CANCELLED,
    AG_LOOP_TOOL_CAPACITY
} ag_loop_status;

typedef enum {
    AG_BACKEND_PENDING = 0,
    AG_BACKEND_READY = 1,
    AG_BACKEND_FAILED = -1
} ag_backend_poll_status;

typedef enum {
    AG_LOOP_INPUT_TASK = 0,
    AG_LOOP_INPUT_TOOL_RECEIPT
} ag_loop_input_kind;

typedef struct {
    ag_loop_input_kind kind;
    const char *task;
    const char *call_id;
    ag_code receipt_code;
    const char *receipt_text;
    /* Immutable across a request; increments for each new submitted task. */
    uint64_t generation;
    unsigned turn;
} ag_loop_input;

/* poll must return promptly after at most work_quota units; it may return PENDING. */
typedef ag_backend_poll_status (*ag_backend_poll_fn)(
    void *context, const ag_loop_input *input, uint32_t work_quota,
    char *response, size_t response_capacity, size_t *response_bytes);

typedef ag_backend_poll_status (*ag_backend_poll_v2_fn)(
    void *context, const ag_loop_input *input, uint32_t work_quota,
    char *response, size_t response_capacity, size_t *response_bytes,
    uint64_t *result_generation, unsigned *result_turn);

typedef struct {
    ag_backend_poll_fn poll;
    void *context;
} ag_loop_backend;

typedef enum {
    AG_LOOP_EVENT_TOOL = 1,
    AG_LOOP_EVENT_FINAL,
    AG_LOOP_EVENT_CLARIFICATION,
    AG_LOOP_EVENT_ERROR
} ag_loop_event_kind;

typedef void (*ag_loop_event_fn)(void *context, ag_loop_event_kind kind,
                                 const char *id, ag_code code,
                                 const char *text);

typedef void (*ag_loop_event_v2_fn)(void *context,uint64_t generation,unsigned turn,
                                    ag_loop_event_kind kind,const char *id,
                                    ag_code code,const char *text);

typedef struct {
    ag_runtime runtime;
    ag_loop_backend backend;
    ag_loop_event_fn event;
    void *event_context;
    ag_loop_event_v2_fn event_v2;
    void *event_v2_context;
    char task[AG_LOOP_TASK_MAX + 1];
    char pending_text[AG_LOOP_OUTPUT_MAX + 1];
    char last_call_id[AG_ID_MAX + 1];
    ag_code last_receipt_code;
    unsigned has_receipt;
    unsigned turns;
    unsigned max_turns;
    unsigned backend_polls;
    uint64_t deadline_ms;
    uint64_t generation;
    ag_loop_status status;
    unsigned backend_poll_limit;
    unsigned backend_pending;
    unsigned awaiting_quiescence;
    unsigned pending_at_abort;
    ag_backend_poll_v2_fn poll_v2;
    void *poll_v2_context;
} ag_loop;

void ag_loop_init(ag_loop *loop, ag_loop_backend backend,
                  ag_loop_event_fn event, void *event_context,
                  ag_commit_fn commit, void *commit_context);
/* Install before the first submission. Every non-PENDING completion (READY, FAILED
 * or unknown) must echo both generation and turn captured with that result,
 * never current input tags copied onto a cached completion. Missing or stale
 * echoes fail closed; the backend must discard late results.
 * The legacy poll callback remains unchanged and cannot provide this guarantee. */
int ag_loop_set_backend_v2(ag_loop *loop, ag_backend_poll_v2_fn poll, void *context);
int ag_loop_set_event_v2(ag_loop *loop,ag_loop_event_v2_fn event,void *context);
/* Zero disables the legacy poll-count timeout; the wall-clock deadline remains. */
int ag_loop_set_poll_limit(ag_loop *loop, unsigned max_pending_polls);
/* Call only after the device/cache lease is proven quiescent for this task. */
int ag_loop_ack_quiescent(ag_loop *loop, uint64_t generation);
ag_loop_status ag_loop_submit(ag_loop *loop, const char *task,
                               uint64_t now_ms, uint32_t timeout_ms,
                               unsigned max_turns);
ag_loop_status ag_loop_step(ag_loop *loop, uint64_t now_ms);
void ag_loop_cancel(ag_loop *loop);
const char *ag_loop_status_name(ag_loop_status status);

#endif

