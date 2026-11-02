#ifndef TENSOR_AGENT_COOPERATIVE_BOARD_H
#define TENSOR_AGENT_COOPERATIVE_BOARD_H
#include "ag_loop.h"
/* Optional strong definitions supplied by --ui-source. Default pointers are 0.
 * Called once before each model step; nonzero requests safe-boundary cancel.
 * Polling/UI callbacks must not reenter model inference or ag_loop_step. */
typedef int (*tensor_agent_ui_fn)(void *context, const ag_loop *loop, uint64_t now_ms);
extern tensor_agent_ui_fn tensor_agent_external_ui_hook;
extern void *tensor_agent_external_ui_context;
#endif
