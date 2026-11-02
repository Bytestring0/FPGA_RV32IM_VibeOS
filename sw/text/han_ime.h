#ifndef HAN_IME_H
#define HAN_IME_H
#include <stdint.h>
int han_resources_ready(void);
int han_glyph_index(uint32_t codepoint);
unsigned han_ime_find(const char *pinyin);
const char *han_ime_candidate(unsigned index);
#endif