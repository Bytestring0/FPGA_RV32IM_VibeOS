#include "../sw/agent/ag_loop.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    MODE_FINISH, MODE_NOTE, MODE_EXACT_REPLAY, MODE_REPLAY, MODE_UNKNOWN_TOOL,
    MODE_BAD_SCHEMA, MODE_FAILED, MODE_UNKNOWN_STATUS
} reply_mode;

typedef struct {
    unsigned pending_calls, calls;
    unsigned stale_generation, stale_turn, missing_generation, missing_turn;
    reply_mode mode;
} backend_state;

typedef struct {
    unsigned commits, tools, finals, errors;
    uint64_t event_generation;
    unsigned event_turn,event_count;
    ag_code last_tool_code;
    char last_error[64];
} observer;

static int commit_state(void *context, const ag_state *candidate) {
    observer *seen = (observer *)context;
    assert(candidate != NULL);
    ++seen->commits;
    return 0;
}

static void on_event(void *context, ag_loop_event_kind kind,
                     const char *id, ag_code code, const char *text) {
    observer *seen = (observer *)context;
    if (kind == AG_LOOP_EVENT_TOOL) {
        assert(id[0] != 0);
        ++seen->tools;
        seen->last_tool_code = code;
    } else if (kind == AG_LOOP_EVENT_FINAL) {
        assert(!strcmp(text, "ok"));
        ++seen->finals;
    } else if (kind == AG_LOOP_EVENT_ERROR) {
        size_t length = strlen(text);
        assert(length < sizeof(seen->last_error));
        memcpy(seen->last_error, text, length + 1);
        ++seen->errors;
    } else {
        assert(0 && "unexpected clarification");
    }
}

static void on_event_v2(void *context,uint64_t generation,unsigned turn,
                        ag_loop_event_kind kind,const char *id,ag_code code,const char *text){
    observer *seen=(observer*)context;(void)id;(void)code;(void)text;
    assert(generation!=0&&kind>=AG_LOOP_EVENT_TOOL&&kind<=AG_LOOP_EVENT_ERROR);
    seen->event_generation=generation;seen->event_turn=turn;seen->event_count++;
}

static ag_backend_poll_status poll_v2(void *context, const ag_loop_input *input,
        uint32_t quota, char *response, size_t capacity, size_t *used,
        uint64_t *result_generation, unsigned *result_turn) {
    backend_state *backend = (backend_state *)context;
    assert(quota == 256u && input->generation != 0);
    ++backend->calls;
    if (backend->pending_calls) {
        --backend->pending_calls;
        return AG_BACKEND_PENDING; /* PENDING needs no completion echo. */
    }
    const char *json = "{\"type\":\"finish\",\"text\":\"ok\"}";
    switch (backend->mode) {
    case MODE_NOTE:
    case MODE_EXACT_REPLAY:
        assert(input->kind == (backend->mode == MODE_NOTE
            ? AG_LOOP_INPUT_TASK : AG_LOOP_INPUT_TOOL_RECEIPT));
        assert(input->turn == (backend->mode == MODE_NOTE ? 0u : 1u));
        if (backend->mode == MODE_EXACT_REPLAY)
            assert(!strcmp(input->call_id, "note-1"));
        json = "{\"type\":\"tool\",\"id\":\"note-1\",\"request\":\"{\\\"name\\\":\\\"append_note\\\",\\\"arguments\\\":{\\\"text\\\":\\\"kept\\\"}}\"}";
        break;
    case MODE_REPLAY:
        assert(input->kind == AG_LOOP_INPUT_TOOL_RECEIPT && input->turn == 1);
        assert(!strcmp(input->call_id, "note-1") && input->receipt_code == AG_OK);
        assert(!strcmp(input->receipt_text, "1"));
        json = "{\"type\":\"tool\",\"id\":\"replayed-2\",\"request\":\"{\\\"name\\\":\\\"append_note\\\",\\\"arguments\\\":{\\\"text\\\":\\\"must-not-commit\\\"}}\"}";
        break;
    case MODE_UNKNOWN_TOOL:
        json = "{\"type\":\"tool\",\"id\":\"unknown-1\",\"request\":\"{\\\"name\\\":\\\"flash_erase\\\",\\\"arguments\\\":{}}\"}";
        break;
    case MODE_BAD_SCHEMA:
        json = "{\"type\":\"finish\",\"text\":\"ok\",\"extra\":\"not allowed\"}";
        break;
    default: break;
    }
    size_t length = strlen(json);
    assert(length <= capacity);
    memcpy(response, json, length + 1);
    *used = length;
    if (!backend->missing_generation)
        *result_generation = input->generation - (backend->stale_generation ? 1u : 0u);
    if (!backend->missing_turn)
        *result_turn = backend->stale_turn
            ? (input->turn ? input->turn - 1u : input->turn + 1u) : input->turn;
    if (backend->mode == MODE_FAILED) return AG_BACKEND_FAILED;
    if (backend->mode == MODE_UNKNOWN_STATUS) return (ag_backend_poll_status)42;
    return AG_BACKEND_READY;
}

static void test_same_generation_replay(void) {
    ag_loop loop;
    backend_state backend = {0};
    observer seen = {0};
    ag_loop_init(&loop, (ag_loop_backend){0, 0}, on_event, &seen, commit_state, &seen);
    assert(ag_loop_set_backend_v2(&loop, poll_v2, &backend));
    backend.mode = MODE_NOTE;
    assert(ag_loop_set_event_v2(&loop,on_event_v2,&seen));
    assert(ag_loop_submit(&loop, "save one note", 0, 1000, 4) == AG_LOOP_RUNNING);
    uint64_t generation = loop.generation;
    assert(ag_loop_step(&loop, 1) == AG_LOOP_RUNNING);
    assert(loop.turns == 1 && loop.runtime.state.note_count == 1);
    assert(seen.event_generation==generation&&seen.event_turn==0&&seen.event_count==1);
    assert(loop.runtime.state.record_count == 1 && seen.commits == 1);
    assert(seen.tools == 1 && seen.last_tool_code == AG_OK);
    assert(!strcmp(loop.runtime.state.notes[0], "kept"));

    /* Same generation, prior turn: syntactically valid, distinct side-effecting tool. */
    backend.mode = MODE_REPLAY;
    backend.stale_turn = 1;
    assert(ag_loop_step(&loop, 2) == AG_LOOP_BACKEND_ERROR);
    assert(!strcmp(seen.last_error, "stale_backend_result"));
    assert(seen.event_generation==generation&&seen.event_turn==1&&seen.event_count==2);
    assert(loop.generation == generation && loop.turns == 1);
    assert(loop.awaiting_quiescence && loop.pending_at_abort);
    assert(loop.runtime.state.note_count == 1 && loop.runtime.state.record_count == 1);
    assert(seen.commits == 1 && seen.tools == 1 && seen.finals == 0);
    assert(!strcmp(loop.runtime.state.notes[0], "kept"));
    assert(ag_loop_submit(&loop, "blocked", 3, 1000, 4) == AG_LOOP_PROTOCOL_ERROR);
    assert(!ag_loop_ack_quiescent(&loop, generation + 1));
    assert(ag_loop_ack_quiescent(&loop, generation));
    backend.mode = MODE_FINISH;
    backend.stale_turn = 0;
    assert(ag_loop_submit(&loop, "new task", 4, 1000, 4) == AG_LOOP_RUNNING);
    assert(loop.generation == generation + 1);
    assert(ag_loop_step(&loop, 5) == AG_LOOP_FINISHED && seen.finals == 1);
    assert(loop.runtime.state.note_count == 1 && seen.commits == 1);
}

static void test_exact_prior_tool_response(void) {
    ag_loop loop;
    backend_state backend = {0};
    observer seen = {0};
    ag_loop_init(&loop, (ag_loop_backend){0, 0}, on_event, &seen, commit_state, &seen);
    assert(ag_loop_set_backend_v2(&loop, poll_v2, &backend));
    backend.mode = MODE_NOTE;
    assert(ag_loop_submit(&loop, "exact replay", 0, 100, 4) == AG_LOOP_RUNNING);
    assert(ag_loop_step(&loop, 1) == AG_LOOP_RUNNING);
    uint64_t generation = loop.generation;
    backend.mode = MODE_EXACT_REPLAY; /* Identical payload, same generation, prior turn. */
    backend.stale_turn = 1;
    assert(ag_loop_step(&loop, 2) == AG_LOOP_BACKEND_ERROR);
    assert(loop.generation == generation && loop.turns == 1);
    assert(!strcmp(seen.last_error, "stale_backend_result"));
    assert(loop.awaiting_quiescence && seen.tools == 1);
    assert(loop.runtime.state.note_count == 1 && seen.commits == 1);
    assert(loop.runtime.state.record_count == 1 && seen.finals == 0);
}

static void test_missing_and_stale_echoes(void) {
    ag_loop loop;
    backend_state backend = {0};
    observer seen = {0};
    ag_loop_init(&loop, (ag_loop_backend){0, 0}, on_event, &seen, commit_state, &seen);
    assert(ag_loop_set_backend_v2(&loop, poll_v2, &backend));
    for (unsigned trial = 0; trial < 5; ++trial) {
        backend.mode = trial == 3 ? MODE_FAILED : MODE_FINISH;
        backend.missing_generation = trial == 0;
        backend.missing_turn = trial == 1 || trial == 3;
        backend.stale_generation = trial == 2;
        backend.stale_turn = trial == 4; /* Includes future turn on turn zero. */
        assert(ag_loop_submit(&loop, "fresh", 10u * trial, 100, 4) == AG_LOOP_RUNNING);
        uint64_t generation = loop.generation;
        assert(ag_loop_step(&loop, 10u * trial + 1) == AG_LOOP_BACKEND_ERROR);
        assert(!strcmp(seen.last_error, "stale_backend_result"));
        assert(loop.awaiting_quiescence && loop.turns == 0);
        assert(seen.tools == 0 && seen.finals == 0 && seen.commits == 0);
        assert(ag_loop_ack_quiescent(&loop, generation));
    }
    backend.missing_generation = backend.missing_turn = 0;
    backend.stale_generation = backend.stale_turn = 0;
    backend.mode = MODE_UNKNOWN_STATUS;
    assert(ag_loop_submit(&loop, "unknown result", 60, 100, 4) == AG_LOOP_RUNNING);
    assert(ag_loop_step(&loop, 61) == AG_LOOP_BACKEND_ERROR);
    assert(!strcmp(seen.last_error, "backend_failed"));
    assert(loop.awaiting_quiescence && seen.commits == 0 && seen.finals == 0);
    assert(ag_loop_ack_quiescent(&loop, loop.generation));
}

static void test_cancel_unknown_tool_and_json(void) {
    ag_loop loop;
    backend_state backend = {0};
    observer seen = {0};
    ag_loop_init(&loop, (ag_loop_backend){0, 0}, on_event, &seen, commit_state, &seen);
    assert(ag_loop_set_backend_v2(&loop, poll_v2, &backend));
    assert(ag_loop_set_poll_limit(&loop, 0));
    backend.pending_calls = 140;
    assert(ag_loop_submit(&loop, "pending", 0, 1000, 4) == AG_LOOP_RUNNING);
    uint64_t first = loop.generation;
    for (unsigned i = 0; i < 140; ++i)
        assert(ag_loop_step(&loop, i + 1) == AG_LOOP_RUNNING);
    assert(loop.backend_polls == 140 && loop.generation == first);
    ag_loop_cancel(&loop);
    assert(loop.status == AG_LOOP_CANCELLED && loop.awaiting_quiescence);
    unsigned calls_after_cancel = backend.calls;
    assert(ag_loop_step(&loop, 141) == AG_LOOP_CANCELLED && backend.calls == calls_after_cancel);
    assert(ag_loop_submit(&loop, "blocked", 142, 1000, 4) == AG_LOOP_PROTOCOL_ERROR);
    assert(!ag_loop_ack_quiescent(&loop, first + 1));
    assert(ag_loop_ack_quiescent(&loop, first));

    backend.mode = MODE_UNKNOWN_TOOL;
    assert(ag_loop_submit(&loop, "unknown tool", 200, 1000, 4) == AG_LOOP_RUNNING);
    assert(ag_loop_step(&loop, 201) == AG_LOOP_RUNNING);
    assert(seen.tools == 1 && seen.last_tool_code == AG_UNSUPPORTED);
    assert(loop.runtime.state.record_count == 0 && loop.runtime.state.note_count == 0);
    assert(seen.commits == 0 && loop.last_receipt_code == AG_UNSUPPORTED);
    backend.mode = MODE_FINISH;
    assert(ag_loop_step(&loop, 202) == AG_LOOP_FINISHED && seen.finals == 1);

    /* Echo validation must not weaken the strict outer tool JSON schema. */
    backend.mode = MODE_BAD_SCHEMA;
    assert(ag_loop_submit(&loop, "strict JSON", 203, 1000, 4) == AG_LOOP_RUNNING);
    assert(ag_loop_step(&loop, 204) == AG_LOOP_PROTOCOL_ERROR);
    assert(!strcmp(seen.last_error, "invalid_response_json_or_schema"));
    assert(seen.commits == 0 && seen.finals == 1);
}

static ag_backend_poll_status legacy_poll(void *context, const ag_loop_input *input,
        uint32_t quota, char *response, size_t capacity, size_t *used) {
    unsigned *calls = (unsigned *)context;
    const char *json = "{\"type\":\"finish\",\"text\":\"ok\"}";
    size_t length = strlen(json);
    assert(quota == 256u && input->turn == 0 && length <= capacity);
    ++*calls;
    memcpy(response, json, length + 1);
    *used = length;
    return AG_BACKEND_READY;
}

static void test_legacy_poll_compatibility(void) {
    ag_loop loop;
    unsigned calls = 0;
    observer seen = {0};
    ag_loop_init(&loop, (ag_loop_backend){legacy_poll, &calls}, on_event, &seen, 0, 0);
    assert(ag_loop_submit(&loop, "legacy", 0, 100, 4) == AG_LOOP_RUNNING);
    assert(ag_loop_step(&loop, 1) == AG_LOOP_FINISHED);
    assert(calls == 1 && seen.finals == 1 && seen.errors == 0);
}

typedef struct {ag_loop *loop;observer seen;unsigned queued;} replacement_observer;
static void queue_replacement(void *context,ag_loop_event_kind kind,const char *id,
                              ag_code code,const char *text){
    replacement_observer *o=context;(void)id;(void)code;
    assert(kind==AG_LOOP_EVENT_FINAL&&!strcmp(text,"ok"));
    if(!o->queued){o->queued=1;assert(ag_loop_submit(o->loop,"replacement",2,100,4)==AG_LOOP_RUNNING);}
}
static void capture_original_event(void *context,uint64_t generation,unsigned turn,
                                   ag_loop_event_kind kind,const char *id,ag_code code,const char *text){
    replacement_observer *o=context;
    assert(generation==1&&turn==0&&kind==AG_LOOP_EVENT_FINAL&&!strcmp(text,"ok"));
    assert(o->loop->generation==2&&o->loop->pending_text[0]==0);
    on_event_v2(&o->seen,generation,turn,kind,id,code,text);
}
static void test_event_replacement_identity(void){
    ag_loop loop;backend_state backend={0};replacement_observer o={0};o.loop=&loop;
    ag_loop_init(&loop,(ag_loop_backend){0,0},queue_replacement,&o,0,0);
    assert(ag_loop_set_backend_v2(&loop,poll_v2,&backend));
    assert(ag_loop_set_event_v2(&loop,capture_original_event,&o));
    assert(ag_loop_submit(&loop,"original",0,100,4)==AG_LOOP_RUNNING);
    assert(ag_loop_step(&loop,1)==AG_LOOP_RUNNING);
    assert(o.queued&&o.seen.event_count==1&&o.seen.event_generation==1&&loop.generation==2);
}
int main(void) {
    test_event_replacement_identity();
    test_same_generation_replay();
    test_exact_prior_tool_response();
    test_missing_and_stale_echoes();
    test_cancel_unknown_tool_and_json();
    test_legacy_poll_compatibility();
    puts("AG LOOP SAFETY PASS: replay, missing/stale echoes, quiescence, UNKNOWN, strict JSON, legacy");
    return 0;
}
