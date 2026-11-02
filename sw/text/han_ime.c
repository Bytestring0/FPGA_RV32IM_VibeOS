#include "han_ime.h"
#ifdef HAN_IME_HOST
extern uint32_t han_resource_read32(uint32_t offset);
#else
static uint32_t han_resource_read32(uint32_t o){return *(volatile const uint32_t *)(uintptr_t)(0x10010000u+o);}
#endif
static int initialized;
static uint32_t glyphs,unicode_base,syllables,syllable_base,phrases,phrase_base,used;
static uint32_t candidate_base,candidate_count;
static char word[32];
static uint16_t cached_cp[32],cached_index[32];
static unsigned cache_next;
static uint32_t r32(uint32_t o){return han_resource_read32(o);}
static uint16_t r16(uint32_t o){return (uint16_t)(r32(o&~3u)>>((o&2u)*8));}
static unsigned r8(uint32_t o){return (unsigned)((r32(o&~3u)>>((o&3u)*8))&255u);}
int han_resources_ready(void){
 if(initialized)return initialized>0;
 initialized=-1;
 if(r32(0)!=0x314d4948u||r32(4)!=1)return 0;
 glyphs=r32(8);unicode_base=r32(12);syllables=r32(16);syllable_base=r32(20);phrases=r32(24);phrase_base=r32(28);used=r32(32);
 if(!glyphs||glyphs>4096||syllables>1024||phrases>1024||used>32768||used<48||unicode_base<48||unicode_base>used||2*glyphs>used-unicode_base||syllable_base<48||syllable_base>used||12*syllables>used-syllable_base||phrase_base<48||phrase_base>used||8*phrases>used-phrase_base)return 0;
 initialized=1;return 1;
}
int han_glyph_index(uint32_t cp){
 if(cp>65535||!han_resources_ready())return -1;
 for(unsigned i=0;i<32;i++)if(cached_cp[i]==cp&&cp)return cached_index[i];
 unsigned lo=0,hi=glyphs;
 while(lo<hi){unsigned m=lo+(hi-lo)/2;unsigned v=r16(unicode_base+2*m);if(v<cp)lo=m+1;else hi=m;}
 if(lo==glyphs||r16(unicode_base+2*lo)!=cp)return -1;
 cached_cp[cache_next]=cp;cached_index[cache_next]=lo;cache_next=(cache_next+1)%32;return (int)lo;
}
static int compare(const char *s,uint32_t off){
 for(unsigned i=0;i<32;i++){
  if(off+i>=used)return 1;
  unsigned a=(unsigned char)s[i],b=r8(off+i);
  if(a!=b)return a<b?-1:1;
  if(!a)return 0;
 }
 return 1;
}
unsigned han_ime_find(const char *s){
 candidate_count=0;candidate_base=0;
 if(!han_resources_ready()||!s[0])return 0;
 unsigned lo=0,hi=syllables;
 while(lo<hi){unsigned m=lo+(hi-lo)/2;uint32_t p=r32(syllable_base+12*m);int c=compare(s,p);if(c>0)lo=m+1;else hi=m;}
 if(lo==syllables||compare(s,r32(syllable_base+12*lo)))return 0;
 candidate_base=r32(syllable_base+12*lo+4);candidate_count=r32(syllable_base+12*lo+8);
 if(candidate_count>1024||candidate_base>=used||2*candidate_count>used-candidate_base)candidate_count=0;
 return candidate_count;
}
const char *han_ime_candidate(unsigned ix){
 word[0]=0;
 if(ix>=candidate_count)return word;
 unsigned token=r16(candidate_base+2*ix),len=1;uint32_t ptr=0;
 if(token&0x8000){unsigned id=token&0x7fff;if(id>=phrases)return word;ptr=r32(phrase_base+8*id);len=r32(phrase_base+8*id+4);if(!len||len>10||ptr>=used||2*len>used-ptr)return word;}
 unsigned n=0;
 for(unsigned i=0;i<len;i++){
  unsigned g=(token&0x8000)?r16(ptr+2*i):token;
  if(g>=glyphs){word[0]=0;return word;}
  unsigned cp=r16(unicode_base+2*g);
  word[n++]=(char)(0xe0|(cp>>12));word[n++]=(char)(0x80|((cp>>6)&63));word[n++]=(char)(0x80|(cp&63));
 }
 word[n]=0;return word;
}
