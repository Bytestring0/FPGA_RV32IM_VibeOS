#include "ag_text_display.h"
#include <string.h>
static uint32_t rd(ag_text_display*d,unsigned off){return d->io.read(d->io.user,AG_TEXT_BASE+off);}
static void wr(ag_text_display*d,unsigned off,uint32_t value){d->io.write(d->io.user,AG_TEXT_BASE+off,value);}
static int wait_state(ag_text_display*d,unsigned visible,uint32_t timeout){
 uint64_t start=d->io.clock(d->io.user);
 for(;;){uint32_t status=rd(d,4);if(status&16u)return -2;if(!(status&4u)&&((status>>1)&1u)==visible)return 0;
  if(d->io.clock(d->io.user)-start>=timeout)return -2;if(d->io.yield)d->io.yield(d->io.user);}
}
int ag_display_init(ag_text_display*d,const ag_display_io*io,ag_glyph_fn glyph,void*user){
 if(!d||!io||!io->read||!io->write||!io->clock||io->clock_hz<100000||!glyph)return -1;
 memset(d,0,sizeof(*d));d->io=*io;d->glyph=glyph;d->glyph_user=user;d->bus_levels=3;
 if(rd(d,28)!=0x54585432u)return -3;return 0;
}
int ag_display_hide(ag_text_display*d,uint32_t timeout){if(!d)return -1;uint32_t s=rd(d,4);
 if(s&4u)return -2;if(!(s&2u))return 0;wr(d,8,0);return wait_state(d,0,timeout);}
int ag_display_present(ag_text_display*d,uint32_t timeout){if(!d)return -1;uint32_t s=rd(d,4);
 if(s&12u)return -2;if(s&2u)return 0;wr(d,8,1);return wait_state(d,1,timeout);}
int ag_display_clear(ag_text_display*d,uint32_t timeout){
 if(!d)return -1;int rc=ag_display_hide(d,timeout);if(rc)return rc;wr(d,8,2);uint64_t start=d->io.clock(d->io.user);
 while(rd(d,4)&8u){if(d->io.clock(d->io.user)-start>=timeout)return -2;if(d->io.yield)d->io.yield(d->io.user);}
 wr(d,12,0);for(unsigned row=0;row<16;row++)wr(d,16,0);
 d->codepoints[0]=32;d->glyph_count=1;d->cursor=0;return 0;
}
static int glyph_slot(ag_text_display*d,uint32_t cp){
 for(unsigned i=0;i<d->glyph_count;i++)if(d->codepoints[i]==cp)return (int)i;
 if(d->glyph_count==256)return -4;unsigned char rows[16];if(d->glyph(d->glyph_user,cp,rows))return -5;
 unsigned slot=d->glyph_count;wr(d,12,slot*16);for(unsigned row=0;row<16;row++)wr(d,16,rows[row]);
 d->codepoints[slot]=cp;d->glyph_count++;return (int)slot;
}
static int next_codepoint(const unsigned char*s,size_t n,size_t*used,uint32_t*cp){
 if(!n)return -1;unsigned bytes;uint32_t value;
 if(s[0]<0x80){bytes=1;value=s[0];}
 else if(s[0]>=0xc2&&s[0]<=0xdf){bytes=2;value=s[0]&31;}
 else if(s[0]>=0xe0&&s[0]<=0xef){bytes=3;value=s[0]&15;}
 else if(s[0]>=0xf0&&s[0]<=0xf4){bytes=4;value=s[0]&7;}else return -1;
 if(n<bytes)return -1;for(unsigned i=1;i<bytes;i++){if((s[i]&0xc0)!=0x80)return -1;value=(value<<6)|(s[i]&63);}
 if((bytes==2&&value<0x80)||(bytes==3&&value<0x800)||(bytes==4&&value<0x10000)||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return -1;
 *used=bytes;*cp=value;return 0;
}
int ag_display_text(ag_text_display*d,unsigned col,unsigned row,unsigned attr,const char*text,size_t bytes){
 if(!d||(!text&&bytes)||col>=80||row>=30)return -1;if(rd(d,4)&15u)return -2;
 unsigned cursor=row*80+col;wr(d,24,attr&255);wr(d,20,cursor);
 while(bytes){size_t used;uint32_t cp;if(next_codepoint((const unsigned char*)text,bytes,&used,&cp))return -6;
  text+=used;bytes-=used;if(cp==13){cursor-=cursor%80;wr(d,20,cursor);continue;}
  if(cp==10){cursor=(cursor/80+1)*80;if(cursor>=2400)return -4;wr(d,20,cursor);continue;}
  if(cursor>=2400)return -4;int slot=glyph_slot(d,cp);if(slot<0)return slot;wr(d,0,(unsigned)slot);cursor++;
 }
 d->cursor=cursor;return 0;
}
static void delay(ag_text_display*d){uint64_t start=d->io.clock(d->io.user);uint32_t ticks=d->io.clock_hz/200000u;
 while(d->io.clock(d->io.user)-start<ticks){} }
static void lines(ag_text_display*d,unsigned levels){d->bus_levels=(d->bus_levels&4u)|(levels&3u);wr(d,32,d->bus_levels);delay(d);}
static int raise_clock(ag_text_display*d,unsigned sda){lines(d,1u|(sda?2u:0u));uint64_t start=d->io.clock(d->io.user);
 while(!(rd(d,32)&1u)){if(d->io.clock(d->io.user)-start>d->io.clock_hz/100u)return -7;}delay(d);return 0;}
static int start(ag_text_display*d){if(raise_clock(d,1))return -7;if(!(rd(d,32)&2u))return -7;lines(d,1);lines(d,0);return 0;}
static void stop(ag_text_display*d){lines(d,0);(void)raise_clock(d,0);lines(d,3);}
static int send(ag_text_display*d,unsigned byte){for(unsigned mask=128;mask;mask>>=1){unsigned bit=!!(byte&mask);lines(d,bit?2:0);if(raise_clock(d,bit))return -7;lines(d,bit?2:0);}
 lines(d,2);if(raise_clock(d,1))return -7;int nack=!!(rd(d,32)&2u);lines(d,2);return nack?-8:0;}
int ag_hdmi_reset(ag_text_display*d,int released){if(!d)return -1;d->bus_levels=released?7:3;wr(d,32,d->bus_levels);return 0;}
int ag_hdmi_register_write(ag_text_display*d,uint16_t reg,uint8_t value){if(!d)return -1;int rc=start(d);
 if(!rc)rc=send(d,0xb2);if(!rc)rc=send(d,reg&255);if(!rc)rc=send(d,reg>>8);if(!rc)rc=send(d,value);stop(d);return rc;}
int ag_hdmi_register_read(ag_text_display*d,uint16_t reg,uint8_t*out){if(!d||!out)return -1;int rc=start(d);unsigned value=0;
 if(!rc)rc=send(d,0xb2);if(!rc)rc=send(d,reg&255);if(!rc)rc=send(d,reg>>8);if(!rc)rc=start(d);if(!rc)rc=send(d,0xb3);
 for(unsigned i=0;!rc&&i<8;i++){lines(d,2);rc=raise_clock(d,1);value=(value<<1)|!!(rd(d,32)&2u);lines(d,2);}
 if(!rc){lines(d,2);rc=raise_clock(d,1);lines(d,2);}stop(d);if(!rc)*out=(uint8_t)value;return rc;}
/* Register facts: https://github.com/avmwww/ms7210-linux-driver
 * Fixed RGB888 SDR HSVSDE, 720x480p at27MHz. DVI-compatible video over HDMI. */
static int hd_update(ag_text_display*d,uint16_t reg,uint8_t mask,uint8_t bits){
 uint8_t value;int rc=ag_hdmi_register_read(d,reg,&value);if(rc)return rc;
 return ag_hdmi_register_write(d,reg,(uint8_t)((value&~mask)|(bits&mask)));
}
static int hd_word(ag_text_display*d,uint16_t reg,uint16_t word){
 int rc=ag_hdmi_register_write(d,reg,(uint8_t)word);return rc?rc:ag_hdmi_register_write(d,(uint16_t)(reg+1),(uint8_t)(word>>8));
}
static void hd_delay_ms(ag_text_display*d,unsigned ms){uint64_t start=d->io.clock(d->io.user),wait=(uint64_t)d->io.clock_hz*ms/1000u;
 while(d->io.clock(d->io.user)-start<wait){if(d->io.yield)d->io.yield(d->io.user);}}
int ag_ms7210_init_480p(ag_text_display*d,uint32_t timeout){
 if(!d)return -1;uint64_t start_ticks=d->io.clock(d->io.user);int rc=ag_hdmi_reset(d,0);if(rc)return rc;
 hd_delay_ms(d,10);ag_hdmi_reset(d,1);hd_delay_ms(d,20);
 uint8_t id0,id1;if((rc=ag_hdmi_register_read(d,0,&id0))||(rc=ag_hdmi_register_read(d,1,&id1)))return rc;
 if((id0==0&&id1==0)||(id0==255&&id1==255))return -9;
 struct setting{uint16_t reg;uint8_t mask,bits;};
 static const struct setting setup[]={
  {0x0060,255,0x81},{0x0060,255,0x54},{0x0079,0x30,0x20},{0x0080,0x0f,0},
  {0x1202,0xef,0},{0x1201,0x10,0},{0x00c0,3,0},{0x1200,0x0c,4},
  {0x1263,1,0},{0x1265,1,0},{0x1262,1,0},{0x120b,5,4},
  {0x1206,3,3},{0x120b,0x30,0x30},{0x0016,4,4},{0x0009,1,1},
  {0x0007,0x0d,9},{0x0901,0x18,0},{0x092a,0x44,0},{0x090f,7,0},
  {0x0901,1,1},{0x0904,8,8},{0x0008,0xf0,0xf0},{0x000a,0xf0,0xf0},
  {0x0924,0x77,0x44},{0x0925,0x77,0x44},{0x0927,0xf0,0x20},{0x0923,7,7},
  {0x0900,255,0x20},{0x0906,4,0},{0x0920,0x40,0},{0x0926,255,0x11},
  {0x0927,255,1},{0x0928,255,0x11},{0x0929,255,1},{0x0905,3,2},
  {0x0900,7,0},{0x0920,0x2a,0x0a},{0x091f,7,7},{0x0920,4,4},
  {0x000b,0x11,0},{0x0006,0x11,0x11},{0x0503,1,1},{0x0508,0xf7,0},
  {0x0507,2,0},{0x000b,0x11,0x11},{0x0910,255,1}
 };
 for(unsigned i=0;i<sizeof setup/sizeof setup[0];i++){
  if(d->io.clock(d->io.user)-start_ticks>timeout)return -2;
  rc=hd_update(d,setup[i].reg,setup[i].mask,setup[i].bits);if(rc)return rc;
 }
 static const uint16_t timing[]={858,525,125,2,62,6,122,842,36,516,299,539};
 for(unsigned i=0;i<sizeof timing/sizeof timing[0];i++){
  rc=hd_word(d,(uint16_t)(0x120c+i*2),timing[i]);if(rc)return rc;
 }
 hd_delay_ms(d,10);return d->io.clock(d->io.user)-start_ticks>timeout?-2:0;
}

