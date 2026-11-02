#include "qw_tool_normalize.h"
#include <string.h>

typedef struct {
    const char *bytes;
    size_t size, at;
} xml_reader;

typedef struct { char key[24], value[AG_TEXT_MAX + 1]; size_t length; } arg_field;
typedef struct { char name[32]; arg_field fields[3]; unsigned count; } xml_call;
typedef struct { const char *name, *keys[3]; unsigned count, numeric; } tool_schema;

static const tool_schema schemas[] = {
    {"calculate", {"expression", 0, 0}, 1, 0},
    {"save_result", {"result_id", 0, 0}, 1, 0},
    {"append_note", {"text", 0, 0}, 1, 0},
    {"query_state", {0, 0, 0}, 0, 0},
    {"start_timer", {"value", "unit", "label"}, 3, 1},
    {"set_timer", {"value", "unit", 0}, 2, 1},
    {"extend_timer", {"value", "unit", 0}, 2, 1},
    {"pause_timer", {0, 0, 0}, 0, 0},
    {"resume_timer", {0, 0, 0}, 0, 0},
    {"cancel_timer", {0, 0, 0}, 0, 0},
    {"finish", {"text", 0, 0}, 1, 0},
    {"clarify", {"question", 0, 0}, 1, 0}
};

static size_t short_length(const char *s, size_t max) {
    size_t n = 0;
    if (!s) return max + 1;
    while (n <= max && s[n]) ++n;
    return n;
}

static int valid_id(const char *s, size_t max) {
    size_t n = short_length(s, max);
    if (!n || n > max) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Reject NUL, overlong encodings, surrogates and non-XML C0 bytes. */
static int valid_utf8(const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i++];
        uint32_t cp; unsigned more;
        if (c < 0x80) {
            if (c == 0 || (c < 0x20 && c != 9 && c != 10 && c != 13)) return 0;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31u; more = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15u; more = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7u; more = 3; }
        else return 0;
        if (more > n - i) return 0;
        unsigned count = more;
        while (more--) {
            c = (unsigned char)s[i++];
            if ((c & 0xc0u) != 0x80u) return 0;
            cp = (cp << 6) | (uint32_t)(c & 63u);
        }
        if ((count == 1 && cp < 128) || (count == 2 && cp < 2048) ||
            (count == 3 && cp < 65536) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    }
    return 1;
}

static void space(xml_reader *r) {
    while (r->at < r->size && (r->bytes[r->at] == ' ' ||
        (unsigned char)r->bytes[r->at] == 9u ||
        (unsigned char)r->bytes[r->at] == 10u ||
        (unsigned char)r->bytes[r->at] == 13u)) ++r->at;
}
static int take(xml_reader *r, const char *literal) {
    size_t n = strlen(literal);
    if (n > r->size - r->at || memcmp(r->bytes + r->at, literal, n)) return 0;
    r->at += n;
    return 1;
}
static int starts(const xml_reader *r, const char *literal) {
    size_t n = strlen(literal);
    return n <= r->size - r->at && !memcmp(r->bytes + r->at, literal, n);
}
static int tag_name(xml_reader *r, const char *opening, char *dst, size_t cap) {
    size_t n = 0;
    if (!take(r, opening)) return 0;
    while (r->at < r->size && r->bytes[r->at] != '>') {
        unsigned char c = (unsigned char)r->bytes[r->at++];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') ||
            n + 1 >= cap) return 0;
        dst[n++] = (char)c;
    }
    if (!n || r->at == r->size) return 0;
    dst[n] = 0; ++r->at;
    return 1;
}

static int xml_value(xml_reader *r, arg_field *field) {
    size_t start, end, used = 0;
    /* The prompt template puts content on its own line; remove precisely the
       formatting newline, not whitespace belonging to the argument. */
    if (starts(r, "\r\n")) r->at += 2;
    else if (starts(r, "\n")) ++r->at;
    start = r->at;
    while (r->at < r->size && !starts(r, "</parameter>")) {
        if (r->bytes[r->at] == '<' || r->bytes[r->at] == '>') return 0;
        ++r->at;
    }
    if (r->at == r->size) return 0;
    end = r->at;
    if (end > start && (unsigned char)r->bytes[end - 1] == 10u) {
        --end;
        if (end > start && (unsigned char)r->bytes[end - 1] == 13u) --end;
    }
    for (size_t i = start; i < end;) {
        char c = r->bytes[i++];
        if (c == '&') {
            static const struct { const char *entity; char decoded; } entities[] = {
                {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'},
                {"quot;", '"'}, {"apos;", '\''}
            };
            unsigned j;
            for (j = 0; j < sizeof(entities) / sizeof(entities[0]); ++j) {
                size_t n = strlen(entities[j].entity);
                if (n <= end - i && !memcmp(r->bytes + i, entities[j].entity, n)) {
                    i += n; c = entities[j].decoded; break;
                }
            }
            if (j == sizeof(entities) / sizeof(entities[0])) return 0;
        }
        if (used >= AG_TEXT_MAX) return 0;
        field->value[used++] = c;
    }
    field->value[used] = 0; field->length = used;
    return take(r, "</parameter>");
}

static const tool_schema *schema_for(const char *name) {
    for (size_t i = 0; i < sizeof(schemas) / sizeof(schemas[0]); ++i)
        if (!strcmp(schemas[i].name, name)) return &schemas[i];
    return 0;
}
static const arg_field *get_field(const xml_call *c, const char *key) {
    for (unsigned i = 0; i < c->count; ++i)
        if (!strcmp(c->fields[i].key, key)) return &c->fields[i];
    return 0;
}

/* Milliseconds are computed exactly, with no float or locale dependence. */
static int duration_ok(const xml_call *c) {
    const arg_field *v = get_field(c, "value"), *u = get_field(c, "unit");
    size_t i = 0; uint32_t whole = 0, frac = 0, factor = 100;
    unsigned digits = 0; uint64_t mult, millis;
    if (!v || !u || !v->length) return 0;
    if (!strcmp(u->value, "秒")) mult = 1;
    else if (!strcmp(u->value, "分钟")) mult = 60;
    else if (!strcmp(u->value, "小时")) mult = 3600;
    else return 0;
    if (v->value[0] == '0') ++i;
    else {
        if (v->value[0] < '1' || v->value[0] > '9') return 0;
        while (i < v->length && v->value[i] >= '0' && v->value[i] <= '9') {
            if (whole > 86400) return 0;
            whole = whole * 10u + (uint32_t)(v->value[i++] - '0');
            if (whole > 86400) return 0;
        }
    }
    if (i < v->length && v->value[i] == '.') {
        ++i;
        while (i < v->length && v->value[i] >= '0' && v->value[i] <= '9') {
            if (!factor) return 0;
            frac += (uint32_t)(v->value[i++] - '0') * factor;
            factor /= 10; ++digits;
        }
        if (!digits) return 0;
    }
    if (i != v->length) return 0;
    millis = ((uint64_t)whole * 1000u + frac) * mult;
    return millis >= 1000u && millis <= 86400000u;
}

static int parse_xml(const char *text, size_t n, xml_call *c,
                     const tool_schema **schema) {
    xml_reader r = {text, n, 0};
    memset(c, 0, sizeof(*c));
    space(&r);
    if (!take(&r, "<tool_call>")) return 0;
    space(&r);
    if (!tag_name(&r, "<function=", c->name, sizeof(c->name))) return 0;
    *schema = schema_for(c->name);
    if (!*schema) return 0;
    for (;;) {
        space(&r);
        if (!starts(&r, "<parameter=")) break;
        if (c->count >= (*schema)->count || c->count >= 3) return 0;
        arg_field *f = &c->fields[c->count];
        if (!tag_name(&r, "<parameter=", f->key, sizeof(f->key))) return 0;
        for (unsigned i = 0; i < c->count; ++i)
            if (!strcmp(f->key, c->fields[i].key)) return 0;
        if (!xml_value(&r, f) || !f->length ||
            !valid_utf8(f->value, f->length)) return 0;
        ++c->count;
    }
    if (!take(&r, "</function>")) return 0;
    space(&r);
    if (!take(&r, "</tool_call>")) return 0;
    space(&r);
    if (r.at != r.size || c->count != (*schema)->count) return 0;
    for (unsigned i = 0; i < (*schema)->count; ++i) {
        const arg_field *f = get_field(c, (*schema)->keys[i]);
        if (!f) return 0;
        if (!strcmp(f->key, "expression") && f->length > 100) return 0;
        if ((!strcmp(c->name, "finish") || !strcmp(c->name, "clarify")) &&
            f->length > AG_RESULT_MAX) return 0;
    }
    if ((*schema)->numeric && !duration_ok(c)) return 0;
    return 1;
}

typedef struct { char *out; size_t cap, used; int bad; } writer;
static void emit(writer *w, char ch) {
    if (w->bad || w->used + 1 >= w->cap) { w->bad = 1; return; }
    w->out[w->used++] = ch; w->out[w->used] = 0;
}
static void literal(writer *w, const char *s) { while (*s) emit(w, *s++); }
static void quoted(writer *w, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    emit(w, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 92u || ch == '"') { emit(w, (char)92); emit(w, (char)ch); }
        else if (ch == 10u) literal(w, "\\n");
        else if (ch == 13u) literal(w, "\\r");
        else if (ch == 9u) literal(w, "\\t");
        else if (ch < 32) {
            literal(w, "\\u00"); emit(w, hex[ch >> 4]); emit(w, hex[ch & 15]);
        } else emit(w, (char)ch);
    }
    emit(w, '"');
}
static void qstr(writer *w, const char *s) { quoted(w, s, strlen(s)); }

static int wire_id(char id[AG_ID_MAX + 1], uint32_t task, uint32_t call) {
    char reverse[10]; size_t n = 0, k = 0;
    id[k++] = 'c';
    do { reverse[n++] = (char)('0' + task % 10); task /= 10; } while (task);
    while (n) id[k++] = reverse[--n];
    id[k++] = '_'; n = 0;
    do { reverse[n++] = (char)('0' + call % 10); call /= 10; } while (call);
    while (n) id[k++] = reverse[--n];
    if (k > AG_ID_MAX) return 0;
    id[k] = 0; return 1;
}

void qw_tool_normalizer_init(qw_tool_normalizer *state, uint32_t task_key) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->task_key = task_key; state->next_call = 1;
}
const char *qw_tool_native_id(const qw_tool_normalizer *state, const char *wire) {
    if (!state || !wire) return 0;
    for (unsigned i = 0; i < state->count; ++i)
        if (!strcmp(state->calls[i].wire_id, wire))
            return state->calls[i].native_id[0] ? state->calls[i].native_id : 0;
    return 0;
}
const char *qw_tool_wire_id(const qw_tool_normalizer *state, const char *native) {
    if (!state || !native || !*native) return 0;
    for (unsigned i = 0; i < state->count; ++i)
        if (!strcmp(state->calls[i].native_id, native)) return state->calls[i].wire_id;
    return 0;
}

qw_norm_status qw_tool_normalize(qw_tool_normalizer *state,
    const ag_runtime *runtime, const char *generated, size_t generated_bytes,
    const char *native_id, char *output, size_t output_capacity) {
    xml_call call; const tool_schema *schema = 0;
    (void)runtime; /* ag_execute, not the XML adapter, checks save_result IDs. */
    char inner[AG_REQUEST_MAX + 1], id[AG_ID_MAX + 1];
    writer w, outer; int terminal;
    if (output && output_capacity) output[0] = 0;
    if (!state || !state->task_key || !state->next_call || !generated ||
        !output || !output_capacity) return QW_NORM_INVALID;
    if (!generated_bytes || generated_bytes > QW_TOOL_TEXT_MAX ||
        !valid_utf8(generated, generated_bytes) ||
        (native_id && !valid_id(native_id, QW_NATIVE_ID_MAX))) return QW_NORM_INVALID;
    if (!parse_xml(generated, generated_bytes, &call, &schema)) return QW_NORM_INVALID;
    terminal = !strcmp(call.name, "finish") || !strcmp(call.name, "clarify");
    if (terminal && native_id) return QW_NORM_INVALID;
    if (!terminal && native_id && qw_tool_wire_id(state, native_id)) return QW_NORM_CONFLICT;
    if (!terminal && (state->count >= QW_TOOL_MAP_MAX ||
                      !wire_id(id, state->task_key, state->next_call))) return QW_NORM_LIMIT;
    if (output_capacity > AG_LOOP_RESPONSE_MAX + 1u)
        output_capacity = AG_LOOP_RESPONSE_MAX + 1u;
    outer = (writer){output, output_capacity, 0, 0};
    if (terminal) {
        literal(&outer, !strcmp(call.name, "finish") ?
                "{\"type\":\"finish\",\"text\":" :
                "{\"type\":\"clarify\",\"text\":");
        quoted(&outer, call.fields[0].value, call.fields[0].length);
        emit(&outer, '}');
    } else {
        inner[0] = 0; w = (writer){inner, sizeof(inner), 0, 0};
        literal(&w, "{\"name\":"); qstr(&w, call.name); literal(&w, ",\"arguments\":{");
        for (unsigned i = 0; i < schema->count; ++i) {
            const arg_field *field = get_field(&call, schema->keys[i]);
            if (i) emit(&w, ',');
            qstr(&w, schema->keys[i]); emit(&w, ':');
            if (schema->numeric && i == 0) literal(&w, field->value);
            else quoted(&w, field->value, field->length);
        }
        literal(&w, "}}");
        if (w.bad) return QW_NORM_LIMIT;
        literal(&outer, "{\"type\":\"tool\",\"id\":");
        qstr(&outer, id); literal(&outer, ",\"request\":");
        quoted(&outer, inner, w.used); emit(&outer, '}');
    }
    if (outer.bad) { output[0] = 0; return QW_NORM_LIMIT; }
    if (!terminal) {
        qw_tool_identity *m = &state->calls[state->count++];
        strcpy(m->wire_id, id); strcpy(m->tool, call.name);
        if (native_id) strcpy(m->native_id, native_id);
        ++state->next_call;
    }
    return QW_NORM_OK;
}

qw_norm_status qw_tool_format_receipt(const qw_tool_normalizer *state,
    const ag_runtime *runtime, const char *wire_id, const ag_receipt *receipt,
    char *output, size_t output_capacity) {
    const qw_tool_identity *m = 0; size_t n; writer w;
    if (output && output_capacity) output[0] = 0;
    if (!state || !wire_id || !receipt || !output || !output_capacity ||
        !valid_id(wire_id, AG_ID_MAX) || receipt->code < AG_OK ||
        receipt->code > AG_STORAGE) return QW_NORM_INVALID;
    for (unsigned i = 0; i < state->count; ++i)
        if (!strcmp(state->calls[i].wire_id, wire_id)) { m = &state->calls[i]; break; }
    if (!m) return QW_NORM_INVALID;
    n = short_length(receipt->result, AG_RESULT_MAX);
    if (n > AG_RESULT_MAX || !valid_utf8(receipt->result, n)) return QW_NORM_INVALID;
    if (receipt->code == AG_OK) {
        int found = 0;
        if (!runtime) return QW_NORM_INVALID;
        for (unsigned i = 0; i < runtime->state.record_count; ++i) {
            const ag_record *record = &runtime->state.records[i];
            if (strcmp(record->id, wire_id)) continue;
            if (strcmp(record->tool, m->tool) || record->receipt.code != AG_OK ||
                strcmp(record->receipt.result, receipt->result)) return QW_NORM_INVALID;
            found = 1; break;
        }
        if (!found) return QW_NORM_INVALID;
    }
    if (output_capacity > AG_LOOP_RESPONSE_MAX + 1u)
        output_capacity = AG_LOOP_RESPONSE_MAX + 1u;
    w = (writer){output, output_capacity, 0, 0};
    literal(&w, receipt->code == AG_OK ? "{\"ok\":true,\"code\":" :
                                        "{\"ok\":false,\"code\":");
    qstr(&w, ag_code_name(receipt->code));
    literal(&w, ",\"text\":"); quoted(&w, receipt->result, n);
    literal(&w, ",\"call_id\":"); qstr(&w, wire_id);
    if (receipt->code == AG_OK && !strcmp(m->tool, "calculate")) {
        literal(&w, ",\"result_id\":"); qstr(&w, wire_id);
    }
    emit(&w, '}');
    if (w.bad) { output[0] = 0; return QW_NORM_LIMIT; }
    return QW_NORM_OK;
}
