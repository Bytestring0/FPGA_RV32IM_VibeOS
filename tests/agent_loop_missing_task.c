/* Cross-module fault: the fixed task file is absent from the AGFS image. The
 * firmware must observe AGFS_NOT_FOUND through the normal error return and stop
 * without driving the display (no bogus frame), proving the missing-task path
 * is reported rather than hanging. No scripted backend runs because there is no
 * task to submit.
 */
#include "../sw/platform.h"
#include "../sw/drivers/sd_spi.h"
#include "../sw/drivers/char_video.h"
#include "../sw/fs/agfs.h"

static void fail(uint32_t code, const char *msg) {
    puts_uart("AGENT LOOP MISSING TASK FAIL code=");
    puthex(code);
    putchar_uart(' ');
    puts_uart(msg);
    putchar_uart('\n');
    MMIO32(0x10000004u) = code + 1;
    for (;;) {}
}

int main(void) {
    agfs_file file;

    if (sd_spi_card_init() != SD_SPI_OK) fail(1, "sd init");
    if (agfs_init() != AGFS_OK) fail(2, "agfs init");

    if (agfs_open("task.txt", &file) != AGFS_NOT_FOUND)
        fail(3, "missing task must be AGFS_NOT_FOUND");

    /* The display stays reachable (HIDDEN_READY) but is never loaded, so the
     * hardware has nothing to show. */
    if (!char_video_wait_phase(CHAR_PHASE_HIDDEN_READY, 10000000ull))
        fail(4, "hidden ready phase");

    puts_uart("AGENT LOOP MISSING TASK PASS reason=AGFS_NOT_FOUND\n");

    MMIO32(0x10000004u) = 0;
    for (;;) {}
}
