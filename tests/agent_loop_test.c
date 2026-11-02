#include "../sw/agent/ag_loop.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    SCRIPT_GOOD,
    SCRIPT_CLARIFY,
    SCRIPT_BAD_JSON,
    SCRIPT_UNKNOWN,
    SCRIPT_DUP_CONFLICT,
    SCRIPT_INNER_BAD_JSON,
    SCRIPT_BACKEND_FAIL,
    SCRIPT_OVERFLOW,
    SCRIPT_MAX_TURNS,
    SCRIPT_PENDING
} script_kind;

typedef struct {
    script_kind kind;
    unsigned calls;
    unsigned saw_calc;
    unsigned saw_save;
    unsigned saw_conflict;
    unsigned saw_unknown;
    unsigned saw_inner_bad_json;
} scripted_backend;

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); assert(expr); \
} } while (0)

static ag_backend_poll_status emit(char *dst, size_t capacity, size_t *used, const char *text) {
    size_t n = strlen(text);
    if (n > capacity) { *used = capacity + 1; return AG_BACKEND_READY; }
    memcpy(dst, text, n + 1);
    *used = n;
    return AG_BACKEND_READY;
}

static ag_backend_poll_status scripted_poll(void *context, const ag_loop_input *input,
        uint32_t quota, char *response, size_t capacity, size_t *used) {
    scripted_backend *script = context;
    ++script->calls;
    if (input->kind == AG_LOOP_INPUT_TASK) {
        CHECK(input->call_id[0] == 0);
        CHECK(input->receipt_text[0] == 0);
        CHECK(input->receipt_code == AG_OK);
    }
    CHECK(quota > 0);
    if (script->kind == SCRIPT_PENDING) return AG_BACKEND_PENDING;
    if (script->kind == SCRIPT_BACKEND_FAIL) return AG_BACKEND_FAILED;
    if (script->kind == SCRIPT_OVERFLOW) { *used = capacity + 1; return AG_BACKEND_READY; }
    if (script->kind == SCRIPT_BAD_JSON) return emit(response, capacity, used, "{not-json");

    if (script->kind == SCRIPT_CLARIFY)
        return emit(response, capacity, used, "{\"type\":\"clarify\",\"text\":\"请补充日期\"}");
    if (script->kind == SCRIPT_GOOD) {
        if (input->kind == AG_LOOP_INPUT_TASK)
            return emit(response, capacity, used,
                "{\"type\":\"tool\",\"id\":\"calc-1\",\"request\":\"{\\\"name\\\":\\\"calculate\\\",\\\"arguments\\\":{\\\"expression\\\":\\\"19+23\\\"}}\"}");
        if (!strcmp(input->call_id, "calc-1") && input->receipt_code == AG_OK &&
            !strcmp(input->receipt_text, "42")) {
            script->saw_calc = 1;
            return emit(response, capacity, used,
                "{\"type\":\"tool\",\"id\":\"save-1\",\"request\":\"{\\\"name\\\":\\\"save_result\\\",\\\"arguments\\\":{\\\"result_id\\\":\\\"calc-1\\\"}}\"}");
        }
        if (!strcmp(input->call_id, "save-1") && input->receipt_code == AG_OK &&
            !strcmp(input->receipt_text, "1")) {
            script->saw_save = 1;
            return emit(response, capacity, used, "{\"type\":\"finish\",\"text\":\"42 saved\"}");
        }
        return AG_BACKEND_FAILED;
    }
    if (script->kind == SCRIPT_UNKNOWN) {
        if (input->kind == AG_LOOP_INPUT_TASK)
            return emit(response, capacity, used,
                "{\"type\":\"tool\",\"id\":\"unknown-1\",\"request\":\"{\\\"name\\\":\\\"flash_erase\\\",\\\"arguments\\\":{}}\"}");
        CHECK(input->receipt_code == AG_UNSUPPORTED);
        CHECK(!strcmp(input->receipt_text, "unsupported"));
        script->saw_unknown = 1;
        return emit(response, capacity, used, "{\"type\":\"finish\",\"text\":\"recovered\"}");
    }
    if (script->kind == SCRIPT_DUP_CONFLICT) {
        if (input->kind == AG_LOOP_INPUT_TASK)
            return emit(response, capacity, used,
                "{\"type\":\"tool\",\"id\":\"same\",\"request\":\"{\\\"name\\\":\\\"calculate\\\",\\\"arguments\\\":{\\\"expression\\\":\\\"19+23\\\"}}\"}");
        if (input->receipt_code == AG_OK && !strcmp(input->receipt_text, "42"))
            return emit(response, capacity, used,
                "{\"type\":\"tool\",\"id\":\"same\",\"request\":\"{\\\"name\\\":\\\"append_note\\\",\\\"arguments\\\":{\\\"text\\\":\\\"wrong\\\"}}\"}");
        CHECK(input->receipt_code == AG_CONFLICT);
        CHECK(!strcmp(input->receipt_text, "id_conflict"));
        script->saw_conflict = 1;
        return emit(response, capacity, used, "{\"type\":\"finish\",\"text\":\"conflict handled\"}");
    }
    if (script->kind == SCRIPT_INNER_BAD_JSON) {
        if (input->kind == AG_LOOP_INPUT_TASK)
            return emit(response, capacity, used,
                "{\"type\":\"tool\",\"id\":\"bad-inner\",\"request\":\"not-json\"}");
        CHECK(input->receipt_code == AG_BAD_JSON);
        CHECK(!strcmp(input->receipt_text, "invalid_json"));
        script->saw_inner_bad_json = 1;
        return emit(response, capacity, used, "{\"type\":\"finish\",\"text\":\"bad call rejected\"}");
    }
    if (script->kind == SCRIPT_MAX_TURNS) {
        char json[192];
        unsigned id = script->calls;
        (void)snprintf(json, sizeof(json),
            "{\"type\":\"tool\",\"id\":\"m%u\",\"request\":\"{\\\"name\\\":\\\"calculate\\\",\\\"arguments\\\":{\\\"expression\\\":\\\"1+1\\\"}}\"}", id);
        return emit(response, capacity, used, json);
    }
    return AG_BACKEND_FAILED;
}

static void no_event(void *context, ag_loop_event_kind kind, const char *id,
                     ag_code code, const char *text) {
    unsigned *events = context;
    (void)kind; (void)id; (void)code; (void)text;
    ++*events;
}

static ag_loop_status run_until_terminal(ag_loop *loop, uint64_t *now) {
    for (unsigned i = 0; i < AG_LOOP_MAX_TURNS + 2; ++i) {
        ag_loop_status status = ag_loop_step(loop, (*now)++);
        if (status != AG_LOOP_RUNNING) return status;
    }
    return loop->status;
}

static void test_two_tool_receipt_roundtrip(void) {
    ag_loop loop;
    scripted_backend script = {SCRIPT_GOOD};
    unsigned events = 0;
    ag_loop_init(&loop, (ag_loop_backend){scripted_poll, &script}, no_event, &events, 0, 0);
    CHECK(ag_loop_submit(&loop, "请计算19加23并保存结果", 0, 1000, 8) == AG_LOOP_RUNNING);
    uint64_t now = 1;
    CHECK(run_until_terminal(&loop, &now) == AG_LOOP_FINISHED);
    CHECK(script.saw_calc && script.saw_save);
    CHECK(loop.runtime.state.note_count == 1);
    CHECK(!strcmp(loop.runtime.state.notes[0], "42"));
    CHECK(!strcmp(loop.pending_text, "42 saved"));
    CHECK(events == 3);
}

static ag_loop_status run_script(script_kind kind, scripted_backend *out) {
    ag_loop loop;
    scripted_backend script = {.kind = kind};
    ag_loop_init(&loop, (ag_loop_backend){scripted_poll, &script}, 0, 0, 0, 0);
    CHECK(ag_loop_submit(&loop, "fixture task", 0, 1000, 8) == AG_LOOP_RUNNING);
    uint64_t now = 1;
    ag_loop_status status = run_until_terminal(&loop, &now);
    if (out) *out = script;
    return status;
}

static void test_protocol_and_tool_failures(void) {
    scripted_backend script;
    CHECK(run_script(SCRIPT_CLARIFY, 0) == AG_LOOP_CLARIFY);
    CHECK(run_script(SCRIPT_BAD_JSON, 0) == AG_LOOP_PROTOCOL_ERROR);
    CHECK(run_script(SCRIPT_UNKNOWN, &script) == AG_LOOP_FINISHED && script.saw_unknown);
    CHECK(run_script(SCRIPT_DUP_CONFLICT, &script) == AG_LOOP_FINISHED && script.saw_conflict);
    CHECK(run_script(SCRIPT_INNER_BAD_JSON, &script) == AG_LOOP_FINISHED && script.saw_inner_bad_json);
    CHECK(run_script(SCRIPT_BACKEND_FAIL, 0) == AG_LOOP_BACKEND_ERROR);
    CHECK(run_script(SCRIPT_OVERFLOW, 0) == AG_LOOP_RESPONSE_TOO_LONG);
}

static void test_turn_timeout_cancel_and_limits(void) {
    ag_loop loop;
    scripted_backend script = {.kind = SCRIPT_MAX_TURNS};
    ag_loop_init(&loop, (ag_loop_backend){scripted_poll, &script}, 0, 0, 0, 0);
    CHECK(ag_loop_submit(&loop, "loop", 0, 100, 2) == AG_LOOP_RUNNING);
    CHECK(ag_loop_step(&loop, 1) == AG_LOOP_RUNNING);
    CHECK(ag_loop_step(&loop, 2) == AG_LOOP_RUNNING);
    CHECK(ag_loop_step(&loop, 3) == AG_LOOP_MAX_TURNS_REACHED);

    script = (scripted_backend){.kind = SCRIPT_PENDING};
    ag_loop_init(&loop, (ag_loop_backend){scripted_poll, &script}, 0, 0, 0, 0);
    CHECK(ag_loop_submit(&loop, "wait", 0, 10, 4) == AG_LOOP_RUNNING);
    CHECK(ag_loop_step(&loop, 1) == AG_LOOP_RUNNING);
    CHECK(ag_loop_step(&loop, 10) == AG_LOOP_TIMEOUT);

    ag_loop_init(&loop, (ag_loop_backend){scripted_poll, &script}, 0, 0, 0, 0);
    CHECK(ag_loop_submit(&loop, "cancel", 0, 100, 4) == AG_LOOP_RUNNING);
    ag_loop_cancel(&loop);
    CHECK(loop.status == AG_LOOP_CANCELLED);
    CHECK(ag_loop_step(&loop, 1) == AG_LOOP_CANCELLED);

    ag_loop_init(&loop, (ag_loop_backend){scripted_poll, &script}, 0, 0, 0, 0);
    CHECK(ag_loop_submit(&loop, "poll budget", 0, 1000, 4) == AG_LOOP_RUNNING);
    for (unsigned i = 0; i + 1 < AG_LOOP_BACKEND_POLLS_PER_TURN; ++i)
        CHECK(ag_loop_step(&loop, i) == AG_LOOP_RUNNING);
    CHECK(ag_loop_step(&loop, AG_LOOP_BACKEND_POLLS_PER_TURN) == AG_LOOP_TIMEOUT);

    CHECK(ag_loop_status_name(AG_LOOP_FINISHED)[0] == 'f');
    CHECK(ag_loop_submit(&loop, "", 0, 10, 1) == AG_LOOP_PROTOCOL_ERROR);
}

static void test_new_task_clears_error_receipt(void) {
    ag_loop loop;
    scripted_backend script = {.kind = SCRIPT_UNKNOWN};
    ag_loop_init(&loop, (ag_loop_backend){scripted_poll, &script}, 0, 0, 0, 0);
    CHECK(ag_loop_submit(&loop, "unsupported tool", 0, 1000, 8) == AG_LOOP_RUNNING);
    uint64_t now = 1;
    CHECK(run_until_terminal(&loop, &now) == AG_LOOP_FINISHED);
    CHECK(loop.last_receipt_code == AG_UNSUPPORTED);
    script.kind = SCRIPT_CLARIFY;
    CHECK(ag_loop_submit(&loop, "next task", now, 1000, 8) == AG_LOOP_RUNNING);
    CHECK(ag_loop_step(&loop, now + 1) == AG_LOOP_CLARIFY);
}

int main(void) {
    test_new_task_clears_error_receipt();
    test_two_tool_receipt_roundtrip();
    test_protocol_and_tool_failures();
    test_turn_timeout_cancel_and_limits();
    printf("Agent loop host PASS checks=%u fixed_capacity=%zu bytes\n", checks, sizeof(ag_loop));
    return 0;
}
