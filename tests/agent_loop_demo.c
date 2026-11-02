/* Agent execution loop integrated into the current SoC, end to end:
 *
 *   TF (SD SPI model) -> RV32 -> AGFS -> read the fixed task file
 *     -> ag_loop with an explicitly-marked SCRIPTED backend
 *        (tool calls "calculate 19+23" -> receipt "42", then "save_result" ->
 *         receipt "1", then "finish"; each next step is only issued after the
 *         backend verifies the previous real receipt)
 *     -> compose a final summary (task + tool results + status)
 *     -> one LOAD -> bytes -> one DISPLAY of the character video.
 *
 * Process logs go to UART; the harness (sim/text_video_soc_main.cpp) checks the
 * accepted character stream and the full 640x480 frame pixel-exactly against
 * resources/agent_loop/summary_screen.txt. Volatile state: no commit callback,
 * no power-loss guarantee, and the scripted backend is labelled as such.
 */
#include "../sw/platform.h"
#include "../sw/drivers/sd_spi.h"
#include "../sw/drivers/char_video.h"
#include "../sw/fs/agfs.h"
#include "../sw/agent/ag_agent.h"
#include "../sw/agent/ag_loop.h"

#define TASK_NAME "task.txt"
#define TASK_BYTES 512u
#define SUMMARY_BYTES 2048u
#define PHASE_WAIT_CYCLES 10000000ull

static ag_loop loop;
static uint8_t task_buf[TASK_BYTES];
static char summary[SUMMARY_BYTES];

static void fail(uint32_t code, const char *msg) {
    puts_uart("AGENT LOOP DEMO FAIL code=");
    puthex(code);
    putchar_uart(' ');
    puts_uart(msg);
    putchar_uart('\n');
    MMIO32(0x10000004u) = code + 1;
    for (;;) {}
}

/* The scripted backend is a deterministic stand-in for the model: it is NOT a
 * real model and holds no power-loss state. It verifies each tool receipt
 * before handing out the next step. */
static ag_backend_poll_status scripted_model(void *context, const ag_loop_input *input,
        uint32_t work_quota, char *response, size_t capacity, size_t *used) {
    (void)context;
    (void)work_quota;
    const char *text;
    if (input->kind == AG_LOOP_INPUT_TASK) {
        text = "{\"type\":\"tool\",\"id\":\"calc-1\",\"request\":\"{\\\"name\\\":\\\"calculate\\\",\\\"arguments\\\":{\\\"expression\\\":\\\"19+23\\\"}}\"}";
    } else if (input->receipt_code == AG_OK && input->call_id[0] == 'c' &&
               input->call_id[1] == 'a' && input->call_id[2] == 'l' &&
               input->receipt_text[0] == '4' && input->receipt_text[1] == '2' && !input->receipt_text[2]) {
        text = "{\"type\":\"tool\",\"id\":\"save-1\",\"request\":\"{\\\"name\\\":\\\"save_result\\\",\\\"arguments\\\":{\\\"result_id\\\":\\\"calc-1\\\"}}\"}";
    } else if (input->receipt_code == AG_OK && input->call_id[0] == 's' &&
               input->call_id[1] == 'a' && input->call_id[2] == 'v' &&
               input->receipt_text[0] == '1' && !input->receipt_text[1]) {
        text = "{\"type\":\"finish\",\"text\":\"42 saved\"}";
    } else {
        return AG_BACKEND_FAILED;
    }
    size_t n = 0;
    while (text[n]) ++n;
    if (n >= capacity) { *used = capacity + 1; return AG_BACKEND_READY; }
    for (size_t i = 0; i <= n; ++i) response[i] = text[i];
    *used = n;
    return AG_BACKEND_READY;
}

static void loop_event(void *context, ag_loop_event_kind kind, const char *id,
                       ag_code code, const char *text) {
    (void)context;
    if (kind == AG_LOOP_EVENT_TOOL) {
        puts_uart("TOOL id="); puts_uart(id); puts_uart(" code=");
        puts_uart(ag_code_name(code)); puts_uart(" result="); puts_uart(text); puts_uart("\n");
    } else if (kind == AG_LOOP_EVENT_FINAL) {
        puts_uart("FINAL "); puts_uart(text); puts_uart("\n");
    } else if (kind == AG_LOOP_EVENT_ERROR) {
        puts_uart("LOOP_ERROR "); puts_uart(text); puts_uart("\n");
    }
}

/* Small unchecked string builders for the fixed-size summary (bounded by the
 * deterministic, small runtime state of this fixture). */
static char *wstr(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *wdec(char *o, uint32_t v) {
    char tmp[10]; uint32_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) *o++ = tmp[--n];
    return o;
}
static char *wnewline(char *o) { *o++ = '\n'; return o; }

static uint32_t compose_summary(const char *task, char *out, uint32_t cap) {
    char *o = out;
    (void)cap;
    o = wstr(o, "AGENT RISC-V DISPLAY UNIT"); o = wnewline(o);
    o = wstr(o, "------------------------"); o = wnewline(o);
    o = wstr(o, "TASK   : "); o = wstr(o, task); o = wnewline(o);
    o = wstr(o, "STATUS : "); o = wstr(o, ag_loop_status_name(loop.status)); o = wnewline(o);
    o = wstr(o, "TOOLS  : "); o = wdec(o, loop.runtime.state.record_count); o = wnewline(o);
    o = wstr(o, "NOTES  : "); o = wdec(o, loop.runtime.state.note_count); o = wnewline(o);
    o = wstr(o, "------------------------"); o = wnewline(o);
    for (uint32_t i = 0; i < loop.runtime.state.record_count; ++i) {
        const ag_record *r = &loop.runtime.state.records[i];
        o = wstr(o, "TOOL "); o = wstr(o, r->id);
        o = wstr(o, " "); o = wstr(o, r->tool);
        o = wstr(o, " "); o = wstr(o, ag_code_name(r->receipt.code));
        o = wstr(o, " "); o = wstr(o, r->receipt.result);
        o = wnewline(o);
    }
    for (uint32_t i = 0; i < loop.runtime.state.note_count; ++i) {
        o = wstr(o, "NOTE["); o = wdec(o, i); o = wstr(o, "] = ");
        o = wstr(o, loop.runtime.state.notes[i]); o = wnewline(o);
    }
    o = wstr(o, "------------------------"); o = wnewline(o);
    o = wstr(o, "FINAL : "); o = wstr(o, loop.pending_text); o = wnewline(o);
    *o = 0;
    return (uint32_t)(o - out);
}

int main(void) {
    agfs_file file;
    agfs_result r;
    uint32_t i, task_len;

    if (sd_spi_card_init() != SD_SPI_OK) fail(1, "sd init");
    if (agfs_init() != AGFS_OK) fail(2, "agfs init");
    r = agfs_open(TASK_NAME, &file);
    if (r != AGFS_OK) fail(3, "task open");
    if (file.size_bytes == 0 || file.size_bytes >= TASK_BYTES) fail(4, "task size");
    if (agfs_read(&file, 0, task_buf, file.size_bytes) != AGFS_OK) fail(5, "task read");
    task_buf[file.size_bytes] = 0;
    task_len = file.size_bytes;

    ag_loop_init(&loop, (ag_loop_backend){scripted_model, 0}, loop_event, 0, 0, 0);
    puts_uart("AGENT LOOP backend=scripted volatile_state=1 task=");
    puts_uart((const char *)task_buf);
    puts_uart("\n");

    if (ag_loop_submit(&loop, (const char *)task_buf, 0, 10000, 4) != AG_LOOP_RUNNING)
        fail(6, "submit");

    ag_loop_status status = AG_LOOP_RUNNING;
    for (uint64_t now = 1; now < 100 && status == AG_LOOP_RUNNING; ++now)
        status = ag_loop_step(&loop, now);

    if (status != AG_LOOP_FINISHED || loop.runtime.state.note_count != 1 ||
        loop.runtime.state.notes[0][0] != '4' || loop.runtime.state.notes[0][1] != '2' ||
        loop.runtime.state.notes[0][2])
        fail(7, "loop did not finish with 42");

    if (!char_video_wait_phase(CHAR_PHASE_HIDDEN_READY, PHASE_WAIT_CYCLES))
        fail(8, "hidden ready phase");

    char_video_cmd(CHAR_CMD_LOAD);
    if (char_video_cmd_timeout()) fail(9, "load cmd timeout");
    if (char_video_cmd_error()) fail(10, "load cmd error");
    if (!char_video_wait_phase(CHAR_PHASE_LOADING, PHASE_WAIT_CYCLES))
        fail(11, "loading phase");

    uint32_t summary_len = compose_summary((const char *)task_buf, summary, SUMMARY_BYTES);
    if (summary_len == 0 || summary_len >= SUMMARY_BYTES) fail(12, "summary len");
    for (i = 0; i < summary_len; ++i) {
        char_video_putc((uint32_t)(unsigned char)summary[i]);
        if (char_video_char_timeout()) fail(13, "char timeout");
    }
    if (char_video_overflow()) fail(14, "overflow");

    char_video_cmd(CHAR_CMD_DISPLAY);
    if (char_video_cmd_timeout()) fail(15, "display cmd timeout");
    if (char_video_cmd_error()) fail(16, "display cmd error");
    if (!char_video_wait_phase(CHAR_PHASE_DISPLAYING, PHASE_WAIT_CYCLES))
        fail(17, "displaying phase");

    puts_uart("AGENT LOOP DEMO PASS tools=2 receipts=2 note=42 status=finished task_bytes=");
    puthex(task_len);
    puts_uart(" summary_bytes=");
    puthex(summary_len);
    putchar_uart('\n');

    MMIO32(0x10000004u) = 0;
    for (;;) {}
}
