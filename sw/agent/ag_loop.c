#include "ag_loop.h"
#include <limits.h>

typedef struct {
    const char *text;
    size_t size;
    size_t at;
} json_parser;

typedef struct {
    char type[16];
    char id[AG_ID_MAX + 1];
    char request[AG_REQUEST_MAX + 1];
    char text[AG_LOOP_OUTPUT_MAX + 1];
} loop_response;

static void copy_bytes(void *destination, const void *source, size_t length) {
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    for (size_t i = 0; i < length; ++i) to[i] = from[i];
}

static void fill_bytes(void *destination, int value, size_t length) {
    unsigned char *to = (unsigned char *)destination;
    for (size_t i = 0; i < length; ++i) to[i] = (unsigned char)value;
}

static int same_text(const char *left, const char *right) {
    if (!left || !right) return 0;
    while (*left && *right && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static size_t bounded_length(const char *text, size_t limit) {
    size_t length = 0;
    if (!text) return limit + 1;
    while (length <= limit && text[length]) ++length;
    return length;
}

static void whitespace(json_parser *p) {
    while (p->at < p->size && (p->text[p->at] == ' ' || p->text[p->at] == '\n' ||
           p->text[p->at] == '\r' || p->text[p->at] == '\t')) ++p->at;
}

static int take(json_parser *p, char token) {
    whitespace(p);
    if (p->at >= p->size || p->text[p->at] != token) return 0;
    ++p->at;
    return 1;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(json_parser *p, uint32_t *value) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (p->at >= p->size) return 0;
        int digit = hex_digit(p->text[p->at++]);
        if (digit < 0) return 0;
        out = (out << 4) | (uint32_t)digit;
    }
    *value = out;
    return 1;
}

static int emit_utf8(char *out, size_t capacity, size_t *used, uint32_t codepoint) {
    unsigned char bytes[4];
    size_t count;
    if (codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return 0;
    if (codepoint < 0x80) { bytes[0] = (unsigned char)codepoint; count = 1; }
    else if (codepoint < 0x800) {
        bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codepoint & 63)); count = 2;
    } else if (codepoint < 0x10000) {
        bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 63));
        bytes[2] = (unsigned char)(0x80 | (codepoint & 63)); count = 3;
    } else {
        bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 63));
        bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 63));
        bytes[3] = (unsigned char)(0x80 | (codepoint & 63)); count = 4;
    }
    if (*used + count >= capacity) return 0;
    for (size_t i = 0; i < count; ++i) out[(*used)++] = (char)bytes[i];
    return 1;
}

static int read_json_string(json_parser *p, char *out, size_t capacity) {
    size_t used = 0;
    if (!take(p, '"') || capacity == 0) return 0;
    while (p->at < p->size) {
        unsigned char c = (unsigned char)p->text[p->at++];
        if (c == '"') { out[used] = 0; return 1; }
        if (c < 0x20) return 0;
        if (c == '\\') {
            if (p->at >= p->size) return 0;
            char escaped = p->text[p->at++];
            if (escaped == '"' || escaped == '\\' || escaped == '/') c = (unsigned char)escaped;
            else if (escaped == 'b') c = '\b';
            else if (escaped == 'f') c = '\f';
            else if (escaped == 'n') c = '\n';
            else if (escaped == 'r') c = '\r';
            else if (escaped == 't') c = '\t';
            else if (escaped == 'u') {
                uint32_t cp;
                if (!read_hex4(p, &cp)) return 0;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    uint32_t low;
                    if (p->at + 2 > p->size || p->text[p->at++] != '\\' ||
                        p->text[p->at++] != 'u' || !read_hex4(p, &low) ||
                        low < 0xdc00 || low > 0xdfff) return 0;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return 0;
                if (!emit_utf8(out, capacity, &used, cp)) return 0;
                continue;
            } else return 0;
        }
        if (used + 1 >= capacity) return 0;
        out[used++] = (char)c;
    }
    return 0;
}

static int parse_response(const char *json, size_t size, loop_response *out) {
    json_parser p = {json, size, 0};
    unsigned seen = 0;
    if (!take(&p, '{')) return 0;
    whitespace(&p);
    if (p.at < p.size && p.text[p.at] == '}') return 0;
    for (;;) {
        char key[16];
        if (!read_json_string(&p, key, sizeof(key)) || !take(&p, ':')) return 0;
        unsigned bit;
        char *target;
        size_t capacity;
        if (same_text(key, "type")) { bit = 1; target = out->type; capacity = sizeof(out->type); }
        else if (same_text(key, "id")) { bit = 2; target = out->id; capacity = sizeof(out->id); }
        else if (same_text(key, "request")) { bit = 4; target = out->request; capacity = sizeof(out->request); }
        else if (same_text(key, "text")) { bit = 8; target = out->text; capacity = sizeof(out->text); }
        else return 0;
        if (seen & bit || !read_json_string(&p, target, capacity)) return 0;
        seen |= bit;
        whitespace(&p);
        if (p.at < p.size && p.text[p.at] == '}') { ++p.at; break; }
        if (!take(&p, ',')) return 0;
    }
    whitespace(&p);
    if (p.at != p.size) return 0;
    if (same_text(out->type, "tool")) return seen == 7u && out->id[0] && out->request[0];
    if (same_text(out->type, "finish") || same_text(out->type, "clarify"))
        return seen == 9u;
    return 0;
}

static void emit_event(ag_loop *loop,ag_loop_event_kind kind,unsigned turn,
                       const char *id,ag_code code,const char *text){
    uint64_t generation=loop->generation;
    ag_loop_event_fn legacy=loop->event;void *legacy_context=loop->event_context;
    ag_loop_event_v2_fn v2=loop->event_v2;void *v2_context=loop->event_v2_context;
    char event_id[AG_ID_MAX+1],event_text[AG_LOOP_OUTPUT_MAX+1];
    size_t id_bytes=bounded_length(id,AG_ID_MAX),text_bytes=bounded_length(text,AG_LOOP_OUTPUT_MAX);
    if(id_bytes>AG_ID_MAX||text_bytes>AG_LOOP_OUTPUT_MAX){
        id="";text="event_payload_out_of_bounds";id_bytes=0;text_bytes=27;
        kind=AG_LOOP_EVENT_ERROR;code=AG_BAD_ARGS;
    }
    copy_bytes(event_id,id,id_bytes+1);copy_bytes(event_text,text,text_bytes+1);
    if(legacy)legacy(legacy_context,kind,event_id,code,event_text);
    if(v2)v2(v2_context,generation,turn,kind,event_id,code,event_text);
}
static ag_loop_status fail(ag_loop *loop, ag_loop_status status, const char *reason) {
    if (loop->backend_pending) {
        loop->awaiting_quiescence = 1;
        loop->pending_at_abort = 1;
    }
    loop->status = status;
    emit_event(loop,AG_LOOP_EVENT_ERROR,loop->turns,"",AG_UNSUPPORTED,reason);
    return status;
}

void ag_loop_init(ag_loop *loop, ag_loop_backend backend,
                  ag_loop_event_fn event, void *event_context,
                  ag_commit_fn commit, void *commit_context) {
    fill_bytes(loop, 0, sizeof(*loop));
    loop->backend = backend;
    loop->event = event;
    loop->event_context = event_context;
    loop->status = AG_LOOP_IDLE;
    loop->backend_poll_limit = AG_LOOP_BACKEND_POLLS_PER_TURN;
    ag_init(&loop->runtime, commit, commit_context);
}

int ag_loop_set_backend_v2(ag_loop *loop, ag_backend_poll_v2_fn poll, void *context) {
    if (!loop || !poll || loop->generation || loop->status != AG_LOOP_IDLE ||
        loop->awaiting_quiescence) return 0;
    loop->poll_v2 = poll;
    loop->poll_v2_context = context;
    return 1;
}

int ag_loop_set_event_v2(ag_loop *loop,ag_loop_event_v2_fn event,void *context){
    if(!loop||!event||loop->generation||loop->status!=AG_LOOP_IDLE)return 0;
    loop->event_v2=event;loop->event_v2_context=context;return 1;
}

int ag_loop_set_poll_limit(ag_loop *loop, unsigned max_pending_polls) {
    if (!loop || loop->status == AG_LOOP_RUNNING || loop->awaiting_quiescence) return 0;
    loop->backend_poll_limit = max_pending_polls;
    return 1;
}

int ag_loop_ack_quiescent(ag_loop *loop, uint64_t generation) {
    if (!loop || loop->status == AG_LOOP_RUNNING || !loop->awaiting_quiescence ||
        generation != loop->generation || (loop->pending_at_abort && !loop->poll_v2))
        return 0;
    loop->backend_pending = 0;
    loop->awaiting_quiescence = 0;
    loop->pending_at_abort = 0;
    return 1;
}

ag_loop_status ag_loop_submit(ag_loop *loop, const char *task,
                              uint64_t now_ms, uint32_t timeout_ms,
                              unsigned max_turns) {
    size_t length = bounded_length(task, AG_LOOP_TASK_MAX);
    if (!loop || (!loop->backend.poll && !loop->poll_v2) || length == 0 || length > AG_LOOP_TASK_MAX ||
        timeout_ms == 0 || now_ms > UINT64_MAX - timeout_ms ||
        max_turns == 0 || max_turns > AG_LOOP_MAX_TURNS) return AG_LOOP_PROTOCOL_ERROR;
    if (loop->status == AG_LOOP_RUNNING || loop->awaiting_quiescence ||
        loop->generation == UINT64_MAX) return AG_LOOP_PROTOCOL_ERROR;
    ++loop->generation;
    copy_bytes(loop->task, task, length + 1);
    loop->pending_text[0] = 0;
    loop->last_call_id[0] = 0;
    loop->last_receipt_code = AG_OK;
    loop->has_receipt = 0;
    loop->turns = 0;
    loop->backend_polls = 0;
    loop->max_turns = max_turns;
    loop->deadline_ms = now_ms + timeout_ms;
    loop->status = AG_LOOP_RUNNING;
    return loop->status;
}

ag_loop_status ag_loop_step(ag_loop *loop, uint64_t now_ms) {
    if (!loop || loop->status != AG_LOOP_RUNNING) return loop ? loop->status : AG_LOOP_PROTOCOL_ERROR;
    if (now_ms >= loop->deadline_ms) return fail(loop, AG_LOOP_TIMEOUT, "task_timeout");
    if (loop->turns >= loop->max_turns) return fail(loop, AG_LOOP_MAX_TURNS_REACHED, "max_turns");

    ag_loop_input input;
    input.kind = loop->has_receipt ? AG_LOOP_INPUT_TOOL_RECEIPT : AG_LOOP_INPUT_TASK;
    input.task = loop->task;
    input.call_id = loop->last_call_id;
    input.receipt_code = loop->last_receipt_code;
    input.receipt_text = loop->pending_text;
    input.generation = loop->generation;
    input.turn = loop->turns;
    char response[AG_LOOP_RESPONSE_MAX + 1];
    size_t response_bytes = 0;
    uint64_t result_generation = 0;
    unsigned result_turn = UINT_MAX;
    response[0] = 0;
    ag_backend_poll_status result = loop->poll_v2
        ? loop->poll_v2(loop->poll_v2_context, &input, 256u, response,
                        AG_LOOP_RESPONSE_MAX, &response_bytes, &result_generation,
                         &result_turn)
        : loop->backend.poll(loop->backend.context, &input, 256u, response,
                             AG_LOOP_RESPONSE_MAX, &response_bytes);
    if (result == AG_BACKEND_PENDING) {
        loop->backend_pending = 1;
        if (loop->backend_poll_limit &&
            ++loop->backend_polls >= loop->backend_poll_limit)
            return fail(loop, AG_LOOP_TIMEOUT, "backend_poll_budget_exhausted");
        if (!loop->backend_poll_limit) ++loop->backend_polls;
        return loop->status;
    }
    loop->backend_pending = 0;
    loop->backend_polls = 0;
    if (loop->poll_v2 && (result_generation != loop->generation ||
                          result_turn != input.turn)) {
        loop->awaiting_quiescence = 1;
        loop->pending_at_abort = 1;
        return fail(loop, AG_LOOP_BACKEND_ERROR, "stale_backend_result");
    }
    if (result != AG_BACKEND_READY) {
        loop->awaiting_quiescence = 1;
        loop->pending_at_abort = 1;
        return fail(loop, AG_LOOP_BACKEND_ERROR, "backend_failed");
    }
    if (response_bytes > AG_LOOP_RESPONSE_MAX || response[response_bytes] != 0)
        return fail(loop, AG_LOOP_RESPONSE_TOO_LONG, "response_length_invalid");

    loop_response parsed;
    fill_bytes(&parsed, 0, sizeof(parsed));
    if (!parse_response(response, response_bytes, &parsed))
        return fail(loop, AG_LOOP_PROTOCOL_ERROR, "invalid_response_json_or_schema");

    ++loop->turns;
    if (same_text(parsed.type, "tool")) {
        ag_receipt receipt;
        ag_code code = ag_execute(&loop->runtime, parsed.id, parsed.request, now_ms, &receipt);
        size_t text_length = bounded_length(receipt.result, AG_RESULT_MAX);
        if (text_length > AG_RESULT_MAX)
            return fail(loop, AG_LOOP_PROTOCOL_ERROR, "tool_receipt_unterminated");
        copy_bytes(loop->pending_text, receipt.result, text_length + 1);
        copy_bytes(loop->last_call_id, parsed.id, bounded_length(parsed.id, AG_ID_MAX) + 1);
        loop->last_receipt_code = code;
        loop->has_receipt = 1;
        emit_event(loop,AG_LOOP_EVENT_TOOL,loop->turns-1,parsed.id,code,receipt.result);
        return loop->status;
    }
    if (same_text(parsed.type, "finish")) {
        copy_bytes(loop->pending_text, parsed.text, bounded_length(parsed.text, AG_LOOP_OUTPUT_MAX) + 1);
        loop->status = AG_LOOP_FINISHED;
        emit_event(loop,AG_LOOP_EVENT_FINAL,loop->turns-1,"",AG_OK,loop->pending_text);
        return loop->status;
    }
    copy_bytes(loop->pending_text, parsed.text, bounded_length(parsed.text, AG_LOOP_OUTPUT_MAX) + 1);
    loop->status = AG_LOOP_CLARIFY;
    emit_event(loop,AG_LOOP_EVENT_CLARIFICATION,loop->turns-1,"",AG_OK,loop->pending_text);
    return loop->status;
}

void ag_loop_cancel(ag_loop *loop) {
    if (loop && loop->status == AG_LOOP_RUNNING)
        (void)fail(loop, AG_LOOP_CANCELLED, "cancelled");
}

const char *ag_loop_status_name(ag_loop_status status) {
    static const char *names[] = {"idle", "running", "finished", "clarify", "protocol_error",
        "backend_error", "response_too_long", "max_turns", "timeout", "cancelled", "tool_capacity"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}
