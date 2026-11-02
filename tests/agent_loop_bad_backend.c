/* Cross-module fault: the task is read from AGFS normally, but the scripted
 * backend returns a malformed JSON response ("{not-json"). ag_loop_step must
 * classify it as AG_LOOP_PROTOCOL_ERROR and surface a diagnostic over UART --
 * never hang, never mis-parse. No display is driven on a protocol error.
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

static void fail(uint32_t code, const char *msg) {
    puts_uart("AGENT LOOP BAD BACKEND FAIL code=");
    puthex(code);
    putchar_uart(' ');
    puts_uart(msg);
    putchar_uart('\n');
    MMIO32(0x10000004u) = code + 1;
    for (;;) {}
}

static ag_backend_poll_status bad_model(void *context, const ag_loop_input *input,
        uint32_t work_quota, char *response, size_t capacity, size_t *used) {
    (void)context; (void)input; (void)work_quota; (void)capacity;
    const char *text = "{not-json";
    size_t n = 0;
    while (text[n]) ++n;
    for (size_t i = 0; i <= n; ++i) response[i] = text[i];
    *used = n;
    return AG_BACKEND_READY;
}

static void loop_event(void *context, ag_loop_event_kind kind, const char *id,
                       ag_code code, const char *text) {
    (void)context;
    if (kind == AG_LOOP_EVENT_ERROR) {
        puts_uart("LOOP_ERROR "); puts_uart(text); puts_uart("\n");
    } else if (kind == AG_LOOP_EVENT_TOOL) {
        puts_uart("TOOL id="); puts_uart(id); puts_uart(" code=");
        puts_uart(ag_code_name(code)); puts_uart("\n");
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

    ag_loop_init(&loop, (ag_loop_backend){bad_model, 0}, loop_event, 0, 0, 0);
    puts_uart("AGENT LOOP backend=scripted(bad_json) volatile_state=1\n");

    if (ag_loop_submit(&loop, (const char *)task_buf, 0, 10000, 4) != AG_LOOP_RUNNING)
        fail(6, "submit");

    ag_loop_status status = ag_loop_step(&loop, 1);
    if (status != AG_LOOP_PROTOCOL_ERROR) fail(7, "expected protocol_error");

    puts_uart("AGENT LOOP BAD BACKEND PASS status=");
    puts_uart(ag_loop_status_name(status));
    puts_uart("\n");

    MMIO32(0x10000004u) = 0;
    for (;;) {}
}
