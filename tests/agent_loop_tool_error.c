/* Cross-module fault: the scripted backend asks the tool runtime to compute
 * "1/0". ag_execute returns AG_ARITHMETIC with a diagnostic result, and the
 * backend verifies that exact receipt before finishing -- proving a tool error
 * round-trips through the loop instead of being swallowed. Reported on UART.
 */
#include "../sw/platform.h"
#include "../sw/drivers/sd_spi.h"
#include "../sw/fs/agfs.h"
#include "../sw/agent/ag_agent.h"
#include "../sw/agent/ag_loop.h"

#define TASK_NAME "task.txt"
#define TASK_BYTES 512u

static ag_loop loop;
static uint8_t task_buf[TASK_BYTES];
static int saw_arithmetic;

static void fail(uint32_t code, const char *msg) {
    puts_uart("AGENT LOOP TOOL ERROR FAIL code=");
    puthex(code);
    putchar_uart(' ');
    puts_uart(msg);
    putchar_uart('\n');
    MMIO32(0x10000004u) = code + 1;
    for (;;) {}
}

static ag_backend_poll_status err_model(void *context, const ag_loop_input *input,
        uint32_t work_quota, char *response, size_t capacity, size_t *used) {
    (void)context; (void)work_quota;
    const char *text;
    if (input->kind == AG_LOOP_INPUT_TASK) {
        text = "{\"type\":\"tool\",\"id\":\"div-1\",\"request\":\"{\\\"name\\\":\\\"calculate\\\",\\\"arguments\\\":{\\\"expression\\\":\\\"1/0\\\"}}\"}";
    } else if (input->receipt_code == AG_ARITHMETIC && input->call_id[0] == 'd' &&
               input->receipt_text[0] == 'a') {
        text = "{\"type\":\"finish\",\"text\":\"tool error handled\"}";
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
        if (code == AG_ARITHMETIC) saw_arithmetic = 1;
        puts_uart("TOOL id="); puts_uart(id); puts_uart(" code=");
        puts_uart(ag_code_name(code)); puts_uart(" result="); puts_uart(text); puts_uart("\n");
    } else if (kind == AG_LOOP_EVENT_FINAL) {
        puts_uart("FINAL "); puts_uart(text); puts_uart("\n");
    }
}

int main(void) {
    agfs_file file;

    if (sd_spi_card_init() != SD_SPI_OK) fail(1, "sd init");
    if (agfs_init() != AGFS_OK) fail(2, "agfs init");
    if (agfs_open(TASK_NAME, &file) != AGFS_OK) fail(3, "task open");
    if (file.size_bytes == 0 || file.size_bytes >= TASK_BYTES) fail(4, "task size");
    if (agfs_read(&file, 0, task_buf, file.size_bytes) != AGFS_OK) fail(5, "task read");
    task_buf[file.size_bytes] = 0;

    ag_loop_init(&loop, (ag_loop_backend){err_model, 0}, loop_event, 0, 0, 0);
    puts_uart("AGENT LOOP backend=scripted(tool_error) volatile_state=1\n");

    if (ag_loop_submit(&loop, (const char *)task_buf, 0, 10000, 4) != AG_LOOP_RUNNING)
        fail(6, "submit");

    ag_loop_status status = AG_LOOP_RUNNING;
    for (uint64_t now = 1; now < 100 && status == AG_LOOP_RUNNING; ++now)
        status = ag_loop_step(&loop, now);

    if (status != AG_LOOP_FINISHED) fail(7, "expected finished");
    if (!saw_arithmetic) fail(8, "expected arithmetic receipt");
    /* A tool that errors must NOT be committed: the loop surfaced the receipt
     * to the backend (which verified AG_ARITHMETIC before finishing), and the
     * runtime state is left unchanged. */
    if (loop.runtime.state.record_count != 0 || loop.runtime.state.note_count != 0)
        fail(9, "failed tool must not commit");

    puts_uart("AGENT LOOP TOOL ERROR PASS code=arithmetic handled=1 committed=0\n");

    MMIO32(0x10000004u) = 0;
    for (;;) {}
}
