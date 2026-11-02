#ifndef AG_BUTTON_H
#define AG_BUTTON_H

/*
 * Bare-metal driver for the five board user buttons (rtl/button_inputs.sv),
 * exposed through ag_soc as read-clear event latches. Buttons are active-low at
 * the pins but debounced/level-converted to active-high here: level bit i == 1
 * means "button i is pressed".
 *
 * MMIO map:
 *   0x10001210  BUTTON_EVENTS  R  [4:0]=press [9:5]=release [14:10]=long-press
 *                                 [19:15]=level; reading clears the edge latches
 *   0x10001214  BUTTON_LEVEL   R  [4:0]=debounced level (1 = pressed)
 *
 * press/release are one-cycle pulses; long-press fires exactly once per held
 * press (it never auto-repeats). The edge latches accumulate between reads, so
 * a poll misses nothing even if several edges land between two polls.
 */

#include "../platform.h"

#define BUTTON_EVENTS 0x10001210u
#define BUTTON_LEVEL  0x10001214u

#define BTN_PRESS_MASK   0x1fu
#define BTN_RELEASE_MASK (0x1fu << 5)
#define BTN_LONG_MASK    (0x1fu << 10)
#define BTN_LEVEL_MASK   (0x1fu << 15)

/* Board button bit indices (active-low at the pin; inverted by the RTL). */
#define BTN_K0 0u
#define BTN_K1 1u
#define BTN_K2 2u
#define BTN_K3 3u
#define BTN_K4 4u

/* Read-and-clear the accumulated edge events. */
static inline uint32_t button_events(void) {
    return MMIO32(BUTTON_EVENTS);
}

static inline uint32_t button_level(void) {
    return MMIO32(BUTTON_LEVEL);
}

#endif
