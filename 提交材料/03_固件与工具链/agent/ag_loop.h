#ifndef BOARD_AGENT_LOOP_H
#define BOARD_AGENT_LOOP_H

#include "ag_agent.h"

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
    /* At least one bounded work unit actually completed; clears stall budget. */
    AG_BACKEND_PROGRESS = 2,
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
} ag_loop_input;

/* poll must return promptly after at most work_quota units; it may return PENDING. */
typedef ag_backend_poll_status (*ag_backend_poll_fn)(
    void *context, const ag_loop_input *input, uint32_t work_quota,
    char *response, size_t response_capacity, size_t *response_bytes);

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

/* Invoked once on fail/cancel, after poll has returned at a safe boundary.
 * Cancellation does not assert device/DMA quiescence. */
typedef void (*ag_loop_cancel_fn)(void *context, ag_loop_status reason);

typedef struct {
    ag_runtime runtime;
    ag_loop_backend backend;
    ag_loop_cancel_fn cancel;
    void *cancel_context;
    ag_loop_event_fn event;
    void *event_context;
    char task[AG_LOOP_TASK_MAX + 1];
    char pending_text[AG_LOOP_OUTPUT_MAX + 1];
    char last_call_id[AG_ID_MAX + 1];
    ag_code last_receipt_code;
    unsigned has_receipt;
    unsigned turns;
    unsigned max_turns;
    unsigned backend_polls;
    uint64_t deadline_ms;
    uint64_t last_ms;
    ag_loop_status status;
} ag_loop;

void ag_loop_init(ag_loop *loop, ag_loop_backend backend,
                  ag_loop_event_fn event, void *event_context,
                  ag_commit_fn commit, void *commit_context);
void ag_loop_set_cancel(ag_loop *loop, ag_loop_cancel_fn cancel, void *context);
ag_loop_status ag_loop_submit(ag_loop *loop, const char *task,
                              uint64_t now_ms, uint32_t timeout_ms,
                              unsigned max_turns);
ag_loop_status ag_loop_step(ag_loop *loop, uint64_t now_ms);
void ag_loop_cancel(ag_loop *loop);
const char *ag_loop_status_name(ag_loop_status status);

#endif
