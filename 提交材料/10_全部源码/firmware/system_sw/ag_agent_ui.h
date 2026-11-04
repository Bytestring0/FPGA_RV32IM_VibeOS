#ifndef AG_AGENT_UI_H
#define AG_AGENT_UI_H
#include "ag_text_display.h"
#include "ag_loop.h"
#define AG_UI_OUTPUT_BYTES 2048u
#define AG_UI_INPUT_BYTES 192u
typedef struct {
 ag_text_display display;
 unsigned selected,dirty,running,keyboard,key_index,uppercase,coremark_page,escape_state,ignore_lf;
 uint32_t generation_steps;
 size_t input_bytes;
 uint64_t last_draw_ms;
 char stage[80],output[AG_UI_OUTPUT_BYTES],receipt[256],input[AG_UI_INPUT_BYTES];
 size_t output_bytes;
 int error;
} ag_agent_ui;
int ag_agent_ui_init(ag_agent_ui *,const ag_display_io *,ag_glyph_fn,void *);
void ag_agent_ui_stage(ag_agent_ui *,const char *);
void ag_agent_ui_piece(ag_agent_ui *,const char *,size_t);
void ag_agent_ui_receipt(ag_agent_ui *,const char *);
/* 1: submit message; 2: run real CoreMark; -1: cancel at safe boundary. */
int ag_agent_ui_poll(ag_agent_ui *,const ag_loop *,uint64_t now_ms);
const char *ag_agent_ui_task(const ag_agent_ui *);
void ag_agent_ui_trace(ag_agent_ui *);
/* Immediate full-screen progress bar; permille 0..1000. */
void ag_agent_ui_progress(ag_agent_ui *,const char *,unsigned);
void ag_agent_ui_begin(ag_agent_ui *);
void ag_agent_ui_end(ag_agent_ui *,const ag_loop *);
#endif
