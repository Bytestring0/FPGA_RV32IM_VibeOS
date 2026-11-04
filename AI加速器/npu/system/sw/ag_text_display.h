#ifndef AG_TEXT_DISPLAY_H
#define AG_TEXT_DISPLAY_H
#include <stddef.h>
#include <stdint.h>
#define AG_TEXT_BASE 0x10001100u
/* Clock returns increasing CPU ticks. yield services unrelated work only. */
typedef struct {
 void *user;
 uint32_t (*read)(void *,uint32_t);
 void (*write)(void *,uint32_t,uint32_t);
 uint64_t (*clock)(void *);
 void (*yield)(void *);
 uint32_t clock_hz;
} ag_display_io;
typedef int (*ag_glyph_fn)(void *,uint32_t,unsigned char[16]);
typedef struct {
 ag_display_io io;
 ag_glyph_fn glyph;void *glyph_user;
 uint32_t codepoints[256];unsigned glyph_count,cursor;
 uint32_t bus_levels;
} ag_text_display;
int ag_display_init(ag_text_display *,const ag_display_io *,ag_glyph_fn,void *);
int ag_display_hide(ag_text_display *,uint32_t timeout_ticks);
int ag_display_present(ag_text_display *,uint32_t timeout_ticks);
int ag_display_clear(ag_text_display *,uint32_t timeout_ticks);
int ag_display_text(ag_text_display *,unsigned column,unsigned row,unsigned attributes,const char *,size_t);
int ag_hdmi_reset(ag_text_display *,int released);
int ag_ms7210_init_480p(ag_text_display *,uint32_t timeout_ticks);
int ag_hdmi_register_read(ag_text_display *,uint16_t,uint8_t *);
int ag_hdmi_register_write(ag_text_display *,uint16_t,uint8_t);
#endif
