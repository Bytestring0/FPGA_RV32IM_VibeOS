#ifndef AG_TERMINAL_H
#define AG_TERMINAL_H

/*
 * Bare-metal driver for the interactive character terminal in ag_soc
 * (rtl/cpu_text_terminal.sv, instantiated when TERMINAL_ENABLE=1).
 *
 * MMIO map (see rtl/ag_soc.sv terminal decode):
 *   0x10001200  TERM_ADDR    R/W  target cell 0..2399 for the next TERM_DATA
 *   0x10001204  TERM_DATA    W    [7:0]=char, [8]=inverse-video attribute;
 *                                 written at TERM_ADDR with auto-increment
 *   0x10001208  TERM_CMD     W    0=COMMIT (back->front), 1=CLEAR (fill spaces)
 *   0x1000120c  TERM_STATUS  R    [0]=commit_busy [1]=clear_busy [2]=cmd_timeout
 *                                 [3]=wr_timeout [4]=cmd_error [5]=overflow
 *
 * A store to TERM_DATA or TERM_CMD stalls the CPU (dr backpressure) until the
 * terminal accepts it, or until the hardware TERMINAL_WAIT_TIMEOUT bound fires
 * (sticky timeout bits). COMMIT is a blocking back->front copy performed during
 * one vertical-blanking interval; CLEAR fills the back buffer with spaces and
 * does not touch the front buffer, so a caller follows CLEAR with COMMIT.
 *
 * Cells are written to the back buffer; the screen only changes at COMMIT, so a
 * redraw is: clear + draw text + commit.
 */

#include "../platform.h"

#define TERM_ADDR   0x10001200u
#define TERM_DATA   0x10001204u
#define TERM_CMD    0x10001208u
#define TERM_STATUS 0x1000120cu
#define TERM_GLYPH  0x10001218u

#define TERM_CMD_COMMIT 0u
#define TERM_CMD_CLEAR  1u

#define TERM_STATUS_COMMIT_BUSY (1u << 0)
#define TERM_STATUS_CLEAR_BUSY  (1u << 1)
#define TERM_STATUS_CMD_TIMEOUT (1u << 2)
#define TERM_STATUS_WR_TIMEOUT  (1u << 3)
#define TERM_STATUS_CMD_ERROR   (1u << 4)
#define TERM_STATUS_OVERFLOW    (1u << 5)

static inline uint32_t term_status(void) {
    return MMIO32(TERM_STATUS);
}

static inline void term_gotoxy(uint32_t row, uint32_t col) {
    MMIO32(TERM_ADDR) = ((row * 80u) + col) & 0xfffu;
}

/* Write one cell (char + inverse-video attribute) at the current address,
 * auto-advancing to the next cell. Blocking; see the header note. */
static inline void term_putc_attr(uint32_t c, uint32_t attr) {
    if (c & 0x10000u)
        MMIO32(TERM_GLYPH) = ((attr & 1u) << 17) | (c & 0xffffu);
    else
        MMIO32(TERM_DATA) = ((attr & 1u) << 8) | (c & 0xffu);
}

static inline void term_putc(uint32_t c) {
    term_putc_attr(c, 0);
}

/* Write a NUL-terminated ASCII string at the current address. */
static inline void term_puts(const char *s) {
    while (*s) term_putc((uint32_t)(unsigned char)*s++);
}

static inline void term_commit(void) { MMIO32(TERM_CMD) = TERM_CMD_COMMIT; }
static inline void term_clear(void) { MMIO32(TERM_CMD) = TERM_CMD_CLEAR; }

static inline int term_commit_busy(void) {
    return (term_status() & TERM_STATUS_COMMIT_BUSY) != 0;
}
static inline int term_overflow(void) {
    return (term_status() & TERM_STATUS_OVERFLOW) != 0;
}
static inline int term_cmd_error(void) {
    return (term_status() & TERM_STATUS_CMD_ERROR) != 0;
}
static inline int term_cmd_timeout(void) {
    return (term_status() & TERM_STATUS_CMD_TIMEOUT) != 0;
}
static inline int term_wr_timeout(void) {
    return (term_status() & TERM_STATUS_WR_TIMEOUT) != 0;
}

#endif
