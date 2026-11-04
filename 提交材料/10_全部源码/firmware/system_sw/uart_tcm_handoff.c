#include <stdint.h>
#include <stddef.h>
#ifndef HANDOFF_FONT_OFFSET
#error "Specify frozen firmware application font-pointer offset"
#endif
#ifndef HANDOFF_CACHE_RANGES
#error "Specify frozen firmware cache range function address"
#endif
typedef struct {uint32_t offset,bytes;} range;
__attribute__((section(".text.entry"),noreturn))
void uart_tcm_handoff(void*ui,const void*io,void*glyph,void*application){
 (void)ui;(void)io;(void)glyph;
 uint32_t font=*(const uint32_t*)((const unsigned char*)application+HANDOFF_FONT_OFFSET);
 range owned={font-0x80000000u,1048576u};
 int(*clean)(const range*,size_t,uint32_t)=(void*)(uintptr_t)HANDOFF_CACHE_RANGES;
 if(font<0xbe000000u||font>0xbef00000u||clean(&owned,1,10000000u)){
  const char*p="TCM_HANDOFF_FAIL\n";while(*p)*(volatile uint32_t*)0x10000000u=(unsigned char)*p++;
  for(;;)__asm__ volatile("nop":::"memory");
 }
 const char*p="TCM_HANDOFF_ENTER\n";while(*p)*(volatile uint32_t*)0x10000000u=(unsigned char)*p++;
 __asm__ volatile("fence iorw,iorw;fence.i":::"memory");
 ((void(*)(void))(uintptr_t)font)();
 for(;;)__asm__ volatile("nop":::"memory");
}
