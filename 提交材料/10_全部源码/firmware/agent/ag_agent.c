#include "ag_agent.h"

static size_t length(const char *s, size_t max) {
    size_t n = 0; if (!s) return max + 1;
    while (n <= max && s[n]) ++n;
    return n;
}
static void copy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
}
static void clear(void *dst, size_t n) {
    unsigned char *d = dst; while (n--) *d++ = 0;
}
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; } return *a == *b;
}
static void string_copy(char *dst, const char *src) {
    do { *dst++ = *src; } while (*src++);
}
static char *uint_text(char *out, uint64_t value) {
    char digits[20]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
    *out = 0; return out;
}
static ag_code reply(ag_receipt *r, ag_code code, const char *text) {
    r->code = code; string_copy(r->result, text); return code;
}
const char *ag_code_name(ag_code code) {
    static const char *names[] = {"ok", "bad_json", "bad_args", "unsupported",
        "limit", "arithmetic", "conflict", "not_found", "timer_state", "storage"};
    return (unsigned)code < sizeof(names)/sizeof(names[0]) ? names[code] : "unknown";
}

/* A bounded JSON subset: a name and a flat arguments object. No allocation,
 * duplicate keys, unknown envelope fields, trailing data or nested values. */
typedef struct { const char *p; unsigned failed; } parser;
typedef struct { char key[24], value[AG_TEXT_MAX + 1]; unsigned number; } field;
typedef struct { char name[32]; field fields[3]; unsigned count; } call;
static void space(parser *p) {
    while (*p->p == ' ' || *p->p == '\n' || *p->p == '\r' || *p->p == '\t') ++p->p;
}
static int take(parser *p, char c) {
    space(p); if (*p->p != c) { p->failed = 1; return 0; }
    ++p->p; return 1;
}
static unsigned hex4(parser *p) {
    unsigned v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)*p->p;
        unsigned x = c >= '0' && c <= '9' ? c-'0' :
                     c >= 'a' && c <= 'f' ? c-'a'+10 :
                     c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
        if (x == 16) { p->failed = 1; return 0; }
        ++p->p; v = (v << 4) | x;
    } return v;
}
static int utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t value; unsigned n; unsigned char first = *p++;
        if (first < 128) continue;
        if (first >= 0xc2 && first <= 0xdf) { n = 1; value = first & 31; }
        else if (first >= 0xe0 && first <= 0xef) { n = 2; value = first & 15; }
        else if (first >= 0xf0 && first <= 0xf4) { n = 3; value = first & 7; }
        else return 0;
        unsigned count = n;
        while (n--) {
            if ((*p & 0xc0) != 0x80) return 0;
            value = (value << 6) | (*p++ & 63);
        }
        if ((count == 1 && value < 128) || (count == 2 && value < 2048) ||
            (count == 3 && value < 65536) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return 0;
    } return 1;
}
static int json_string(parser *p, char *out, size_t max) {
    size_t n = 0;
    if (!take(p, '"')) return 0;
    while (*p->p && *p->p != '"') {
        unsigned char bytes[4]; unsigned count = 1;
        bytes[0] = (unsigned char)*p->p++;
        if (bytes[0] < 32) return 0;
        if (bytes[0] == '\\') {
            char c = *p->p; if (!c) return 0; ++p->p;
            if (c == '"' || c == '\\' || c == '/') bytes[0] = (unsigned char)c;
            else if (c == 'n') bytes[0] = '\n';
            else if (c == 'r') bytes[0] = '\r';
            else if (c == 't') bytes[0] = '\t';
            else if (c == 'b') bytes[0] = '\b';
            else if (c == 'f') bytes[0] = '\f';
            else if (c == 'u') {
                uint32_t v = hex4(p);
                if (p->failed || !v) return 0; /* embedded NUL is forbidden */
                if (v >= 0xd800 && v <= 0xdbff) {
                    if (p->p[0] != '\\' || p->p[1] != 'u') return 0;
                    p->p += 2;
                    uint32_t low = hex4(p);
                    if (p->failed || low < 0xdc00 || low > 0xdfff) return 0;
                    v = 0x10000 + ((v-0xd800)<<10) + low-0xdc00;
                } else if (v >= 0xdc00 && v <= 0xdfff) return 0;
                if (v < 128) bytes[0] = (unsigned char)v;
                else if (v < 2048) {
                    count = 2; bytes[0] = 0xc0 | (v >> 6); bytes[1] = 0x80 | (v & 63);
                } else if (v < 65536) {
                    count = 3; bytes[0] = 0xe0 | (v >> 12);
                    bytes[1] = 0x80 | ((v >> 6)&63); bytes[2] = 0x80 | (v&63);
                } else {
                    count = 4; bytes[0] = 0xf0 | (v >> 18);
                    bytes[1] = 0x80 | ((v >> 12)&63); bytes[2] = 0x80 | ((v >> 6)&63); bytes[3] = 0x80 | (v&63);
                }
            } else return 0;
        }
        if (n + count > max) return 0;
        for (unsigned i = 0; i < count; ++i) out[n++] = (char)bytes[i];
    }
    out[n] = 0; return take(p, '"') && utf8(out);
}
static int json_number(parser *p, char *out) {
    const char *start = p->p;
    if (*p->p == '-') ++p->p;
    if (*p->p == '0') ++p->p;
    else {
        if (*p->p < '1' || *p->p > '9') return 0;
        while (*p->p >= '0' && *p->p <= '9') ++p->p;
    }
    if (*p->p == '.') {
        ++p->p; if (*p->p < '0' || *p->p > '9') return 0;
        while (*p->p >= '0' && *p->p <= '9') ++p->p;
    }
    /* Exponents are valid JSON but deliberately unsupported in board v0. */
    size_t n = (size_t)(p->p-start); if (n > 24) return 0;
    copy(out, start, n); out[n] = 0; return 1;
}
static int arguments(parser *p, call *c) {
    if (!take(p, '{')) return 0;
    space(p); if (*p->p == '}') { ++p->p; return 1; }
    for (;;) {
        if (c->count == 3) return 0;
        field *f = &c->fields[c->count];
        if (!json_string(p, f->key, sizeof(f->key)-1) || !take(p, ':')) return 0;
        for (unsigned i = 0; i < c->count; ++i)
            if (equal(c->fields[i].key, f->key)) return 0;
        space(p); f->number = *p->p != '"';
        if (f->number ? !json_number(p, f->value) : !json_string(p, f->value, AG_TEXT_MAX)) return 0;
        ++c->count; space(p);
        if (*p->p == '}') { ++p->p; return 1; }
        if (!take(p, ',')) return 0;
    }
}
static int parse_call(const char *json, call *c) {
    parser p = {json, 0}; unsigned seen = 0;
    clear(c, sizeof(*c));
    if (!take(&p, '{')) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        char key[16]; unsigned bit;
        if (!json_string(&p, key, sizeof(key)-1) || !take(&p, ':')) return 0;
        if (equal(key, "name")) { bit=1; if (!json_string(&p,c->name,31)) return 0; }
        else if (equal(key,"arguments")) { bit=2; if (!arguments(&p,c)) return 0; }
        else return 0;
        if (seen & bit) return 0;
        seen |= bit;
        if (i == 0 && !take(&p, ',')) return 0;
    }
    if (!take(&p, '}')) return 0;
    space(&p); return !p.failed && !*p.p && seen == 3;
}
static const char *get(const call *c, const char *key, unsigned number) {
    for (unsigned i=0; i<c->count; ++i)
        if (equal(c->fields[i].key,key) && c->fields[i].number == number) return c->fields[i].value;
    return 0;
}

#define SCALE 1000LL
#define MAX_FIXED 1000000000LL
typedef struct { const char *p; unsigned error; } arithmetic;
static void math_space(arithmetic *p) { while (*p->p == ' ' || *p->p == '\t') ++p->p; }
static int64_t bounded(arithmetic *p, int64_t v) {
    if (v > MAX_FIXED || v < -MAX_FIXED) p->error = 1;
    return v;
}
static int64_t decimal(arithmetic *p) {
    int64_t whole=0, frac=0, factor=100; unsigned digits=0;
    while (*p->p >= '0' && *p->p <= '9') {
        if (whole > 1000000) { p->error=1; return 0; }
        whole=whole*10+*p->p++-'0'; ++digits;
    }
    if (*p->p == '.') {
        ++p->p;
        while (*p->p >= '0' && *p->p <= '9') {
            if (!factor) { p->error=1; return 0; }
            frac+=(*p->p++-'0')*factor; factor/=10; ++digits;
        }
    }
    if (!digits) p->error=1;
    return bounded(p,whole*SCALE+frac);
}
static int64_t expression(arithmetic *p, unsigned depth);
static int64_t primary(arithmetic *p, unsigned depth) {
    math_space(p);
    if (depth > 12) { p->error=1; return 0; }
    if (*p->p == '+' || *p->p == '-') {
        char op=*p->p++; int64_t v=primary(p,depth+1); return op=='-' ? -v : v;
    }
    if (*p->p == '(') {
        ++p->p; int64_t v=expression(p,depth+1); math_space(p);
        if (*p->p != ')') p->error=1; else ++p->p;
        return v;
    } return decimal(p);
}
static int64_t product(arithmetic *p, unsigned depth) {
    int64_t v=primary(p,depth);
    while (!p->error) {
        math_space(p); char op=*p->p;
        if (op!='*' && op!='/') break;
        ++p->p; int64_t b=primary(p,depth);
        if (p->error) break;
        if (op=='*') v=bounded(p,v*b/SCALE);
        else if (!b) p->error=1;
        else v=bounded(p,v*SCALE/b);
    } return v;
}
static int64_t expression(arithmetic *p, unsigned depth) {
    int64_t v=product(p,depth);
    while (!p->error) {
        math_space(p); char op=*p->p;
        if (op!='+' && op!='-') break;
        ++p->p; int64_t b=product(p,depth);
        if (p->error) break;
        v=bounded(p, op=='+' ? v+b : v-b);
    } return v;
}
ag_code ag_calculate(const char *text, char out[AG_RESULT_MAX+1]) {
    if (!text || length(text,100)>100) return AG_ARITHMETIC;
    arithmetic p={text,0}; int64_t value=expression(&p,0); math_space(&p);
    if (p.error || *p.p) return AG_ARITHMETIC;
    char *dest=out; if (value<0) { *dest++='-'; value=-value; }
    dest=uint_text(dest,(uint64_t)(value/SCALE));
    unsigned fraction=(unsigned)(value%SCALE);
    if (fraction) {
        *dest++='.';
        for (unsigned f=100; f; f/=10) { *dest++=(char)('0'+fraction/f); fraction%=f; }
        while (dest[-1]=='0') --dest;
        *dest=0;
    } return AG_OK;
}
void ag_init(ag_runtime *rt, ag_commit_fn fn, void *ctx) {
    clear(rt,sizeof(*rt)); rt->commit=fn; rt->context=ctx;
}
static ag_code commit(ag_runtime *rt) {
    if (rt->commit && rt->commit(rt->context,&rt->scratch)) return AG_STORAGE;
    copy(&rt->state,&rt->scratch,sizeof(rt->state)); return AG_OK;
}
ag_code ag_tick(ag_runtime *rt, uint64_t now) {
    if (rt->state.timer!=AG_RUNNING || now<rt->state.deadline_ms) return AG_OK;
    copy(&rt->scratch,&rt->state,sizeof(rt->state));
    rt->scratch.timer=AG_FINISHED; rt->scratch.remaining_ms=0;
    ++rt->scratch.timer_revision; return commit(rt);
}
static int valid_id(const char *id) {
    size_t n=length(id,AG_ID_MAX); if (!n || n>AG_ID_MAX) return 0;
    for (size_t i=0;i<n;++i) if (!((id[i]>='a'&&id[i]<='z') ||
        (id[i]>='A'&&id[i]<='Z') || (id[i]>='0'&&id[i]<='9') || id[i]=='_' || id[i]=='-')) return 0;
    return 1;
}
static ag_code duration(const call *c, uint64_t *ms) {
    const char *v=get(c,"value",1), *unit=get(c,"unit",0);
    if (!v || !unit) return AG_BAD_ARGS;
    arithmetic p={v,0}; int64_t fixed=decimal(&p);
    if (p.error || *p.p || fixed<=0) return AG_BAD_ARGS;
    uint64_t factor;
    if (equal(unit,"秒")) factor=1;
    else if (equal(unit,"分钟")) factor=60;
    else if (equal(unit,"小时")) factor=3600;
    else return AG_BAD_ARGS;
    *ms=(uint64_t)fixed*factor;
    return *ms>=1000 && *ms<=86400000 ? AG_OK : AG_BAD_ARGS;
}
static ag_code perform(ag_state *s, const call *c, const char *id, uint64_t now, ag_receipt *r) {
    const char *v;
    if (equal(c->name,"calculate")) {
        if (c->count!=1 || !(v=get(c,"expression",0))) return AG_BAD_ARGS;
        return ag_calculate(v,r->result);
    }
    if (equal(c->name,"append_note") || equal(c->name,"save_result")) {
        if (c->count!=1) return AG_BAD_ARGS;
        if (equal(c->name,"append_note")) v=get(c,"text",0);
        else {
            const char *ref=get(c,"result_id",0); v=0;
            if (!ref) return AG_BAD_ARGS;
            for (unsigned i=0;i<s->record_count;++i)
                if (equal(s->records[i].id,ref) && equal(s->records[i].tool,"calculate")) v=s->records[i].receipt.result;
            if (!v) return AG_NOT_FOUND;
        }
        if (!v || !*v) return AG_BAD_ARGS;
        if (s->note_count==AG_NOTES_MAX) return AG_LIMIT;
        string_copy(s->notes[s->note_count++],v);
        uint_text(r->result,s->note_count); return AG_OK;
    }
    if (equal(c->name,"start_timer") || equal(c->name,"set_timer") || equal(c->name,"extend_timer")) {
        unsigned start=equal(c->name,"start_timer");
        if (c->count!=(start ? 3u : 2u)) return AG_BAD_ARGS;
        uint64_t ms; ag_code status=duration(c,&ms); if (status) return status;
        if (start) {
            v=get(c,"label",0); if (!v || !*v) return AG_BAD_ARGS;
            if (s->timer==AG_RUNNING || s->timer==AG_PAUSED) return AG_TIMER_STATE;
            string_copy(s->label,v); s->timer=AG_RUNNING;
        } else {
            if (s->timer!=AG_RUNNING && s->timer!=AG_PAUSED) return AG_TIMER_STATE;
            if (equal(c->name,"extend_timer")) {
                ms+=s->timer==AG_RUNNING ? s->deadline_ms-now : s->remaining_ms;
                if (ms>86400000) return AG_BAD_ARGS;
            }
        }
        if (now>UINT64_MAX-ms) return AG_BAD_ARGS;
        s->remaining_ms=ms; s->deadline_ms=now+ms; ++s->timer_revision;
        uint_text(r->result,ms); return AG_OK;
    }
    if (equal(c->name,"pause_timer") || equal(c->name,"resume_timer") || equal(c->name,"cancel_timer")) {
        if (c->count) return AG_BAD_ARGS;
        if (equal(c->name,"pause_timer")) {
            if (s->timer!=AG_RUNNING) return AG_TIMER_STATE;
            s->remaining_ms=s->deadline_ms-now; s->timer=AG_PAUSED;
        } else if (equal(c->name,"resume_timer")) {
            if (s->timer!=AG_PAUSED) return AG_TIMER_STATE;
            if (now>UINT64_MAX-s->remaining_ms) return AG_BAD_ARGS;
            s->deadline_ms=now+s->remaining_ms; s->timer=AG_RUNNING;
        } else { s->timer=AG_IDLE; s->remaining_ms=0; s->deadline_ms=0; }
        ++s->timer_revision; string_copy(r->result,"ok"); return AG_OK;
    }
    if (equal(c->name,"query_state")) {
        if (c->count) return AG_BAD_ARGS;
        char *o=r->result; string_copy(o,"notes="); o+=6; o=uint_text(o,s->note_count);
        string_copy(o,";timer="); o+=7; o=uint_text(o,s->timer);
        string_copy(o,";remaining_ms="); o+=14;
        uint_text(o,s->timer==AG_RUNNING ? s->deadline_ms-now : s->remaining_ms);
        return AG_OK;
    }
    if (equal(c->name,"finish")) {
        if (c->count) return AG_BAD_ARGS;
        string_copy(r->result,"finished"); return AG_OK;
    }
    if (equal(c->name,"clarify")) {
        if (c->count!=1 || !(v=get(c,"question",0)) || !*v || length(v,AG_RESULT_MAX)>AG_RESULT_MAX) return AG_BAD_ARGS;
        string_copy(r->result,v); return AG_OK;
    }
    (void)id; return AG_UNSUPPORTED;
}
ag_code ag_execute(ag_runtime *rt, const char *id, const char *json, uint64_t now, ag_receipt *r) {
    if (!valid_id(id) || length(json,AG_REQUEST_MAX)>AG_REQUEST_MAX) return reply(r,AG_BAD_ARGS,"invalid_request");
    for (unsigned i=0;i<rt->state.record_count;++i) {
        const ag_record *record=&rt->state.records[i];
        if (equal(record->id,id)) {
            if (!equal(record->request,json)) return reply(r,AG_CONFLICT,"id_conflict");
            copy(r,&record->receipt,sizeof(*r)); return r->code;
        }
    }
    if (rt->state.record_count==AG_RECEIPTS_MAX) return reply(r,AG_LIMIT,"receipt_capacity");
    call c; if (!parse_call(json,&c)) return reply(r,AG_BAD_JSON,"invalid_json");
    copy(&rt->scratch,&rt->state,sizeof(rt->state));
    if (rt->scratch.timer==AG_RUNNING && now>=rt->scratch.deadline_ms) {
        rt->scratch.timer=AG_FINISHED; rt->scratch.remaining_ms=0; ++rt->scratch.timer_revision;
    }
    reply(r,AG_OK,""); ag_code status=perform(&rt->scratch,&c,id,now,r);
    if (status) return reply(r,status,ag_code_name(status));
    ag_record *record=&rt->scratch.records[rt->scratch.record_count++];
    string_copy(record->id,id); string_copy(record->request,json); string_copy(record->tool,c.name);
    copy(&record->receipt,r,sizeof(*r));
    status=commit(rt);
    if (status) return reply(r,status,"commit_failed");
    return AG_OK;
}
