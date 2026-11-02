#ifndef AG_CHAR_VIDEO_H
#define AG_CHAR_VIDEO_H

/*
 * Bare-metal driver for the character-video peripheral in ag_soc
 * (fpga/hdmi/cpu_text_video.sv, instantiated when CHAR_VIDEO_ENABLE=1).
 *
 * MMIO map (see docs/text_display_integration_contract.md, frozen v2):
 *   0x10001100  CHAR_DATA    W   one byte (strb[0]); accepted in LOADING phase
 *   0x10001104  CHAR_STATUS  R   phase / error / cursor / ready / timeout
 *   0x10001108  CHAR_CONTROL W   command (strb[0]): 0=LOAD, 1=DISPLAY
 *
 * A store to CHAR_DATA or CHAR_CONTROL is a single-request handshake: the CPU
 * store stalls (dr backpressure) until the display accepts the byte/command,
 * or until the hardware CHAR_WAIT_TIMEOUT bound expires. Timeouts are sticky
 * status bits (not bus exceptions), so a lost byte is reported, never silently
 * re-sent late, and the CPU always observes its store completing exactly once.
 */

#include "../platform.h"

#define CHAR_DATA    0x10001100u
#define CHAR_STATUS  0x10001104u
#define CHAR_CONTROL 0x10001108u

/* Commands (CHAR_CONTROL[1:0]). */
#define CHAR_CMD_LOAD    0u
#define CHAR_CMD_DISPLAY 1u

/* Phases (CHAR_STATUS[2:0]). */
#define CHAR_PHASE_RESET_CLEAR  0u
#define CHAR_PHASE_HIDDEN_READY 1u
#define CHAR_PHASE_LOAD_WAIT    2u
#define CHAR_PHASE_LOADING      3u
#define CHAR_PHASE_DISPLAY_WAIT 4u
#define CHAR_PHASE_DISPLAYING   5u

/* Status bits (CHAR_STATUS). */
#define CHAR_STATUS_PHASE_MASK   0x7u
#define CHAR_STATUS_CMD_ERROR    (1u << 3)
#define CHAR_STATUS_OVERFLOW     (1u << 4)
#define CHAR_STATUS_CHAR_READY   (1u << 5)
#define CHAR_STATUS_CMD_READY    (1u << 6)
#define CHAR_STATUS_CHAR_TIMEOUT (1u << 7)
#define CHAR_STATUS_CMD_TIMEOUT  (1u << 8)
#define CHAR_STATUS_CURSOR_X_SHIFT 9
#define CHAR_STATUS_CURSOR_X_MASK  0x7fu
#define CHAR_STATUS_CURSOR_Y_SHIFT 16
#define CHAR_STATUS_CURSOR_Y_MASK  0x1fu

static inline uint32_t char_video_status(void) {
    return MMIO32(CHAR_STATUS);
}

static inline uint32_t char_video_phase(void) {
    return char_video_status() & CHAR_STATUS_PHASE_MASK;
}

static inline int char_video_overflow(void) {
    return (char_video_status() & CHAR_STATUS_OVERFLOW) != 0;
}

static inline int char_video_char_timeout(void) {
    return (char_video_status() & CHAR_STATUS_CHAR_TIMEOUT) != 0;
}

static inline int char_video_cmd_timeout(void) {
    return (char_video_status() & CHAR_STATUS_CMD_TIMEOUT) != 0;
}

static inline int char_video_cmd_error(void) {
    return (char_video_status() & CHAR_STATUS_CMD_ERROR) != 0;
}

/* Blocking single-byte store. The hardware bounds the wait; on expiry the
 * sticky CHAR_STATUS_CHAR_TIMEOUT bit is set instead of the byte being lost. */
static inline void char_video_putc(uint32_t c) {
    MMIO32(CHAR_DATA) = c;
}

/* Blocking command store; see char_video_putc() for timeout semantics. */
static inline void char_video_cmd(uint32_t cmd) {
    MMIO32(CHAR_CONTROL) = cmd;
}

/* Poll for a phase with a cycle-bounded timeout. Returns 1 if the phase is
 * reached before `timeout_cycles` elapse, 0 otherwise. Used for the
 * RESET_CLEAR -> HIDDEN_READY clear and the two CDC ack waits, which are fast
 * when the pixel clock is running but can never complete if it is stopped. */
static inline int char_video_wait_phase(uint32_t phase, uint64_t timeout_cycles) {
    uint64_t start = read_cycles();
    while (char_video_phase() != phase) {
        if (read_cycles() - start > timeout_cycles) return 0;
    }
    return 1;
}

#endif
