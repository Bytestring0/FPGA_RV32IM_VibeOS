#include "../sw/ag_text_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint8_t regs[65536];uint32_t levels;uint64_t ticks;unsigned starts,stops,writes,reads;unsigned bits,byte,part;uint16_t address;int active,reading,ack,ack_high,ack_value,sda_low,read_bit,read_done,nack,stuck;} Bus;
static int sda(Bus*b){return !!(b->levels&2)&&!b->sda_low;}
static uint32_t read_bus(void*u,uint32_t a){Bus*b=u;b->ticks++;if(a==AG_TEXT_BASE+28)return 0x54585432;
 if(a==AG_TEXT_BASE+32)return (b->levels&4)|(sda(b)?2:0)|((b->levels&1)&&!b->stuck);return 0;}
static void receive_byte(Bus*b,unsigned value){b->ack_value=!b->nack;
 if(b->part==0){if((value>>1)!=0x59)b->ack_value=0;b->reading=value&1;b->part=1;}
 else if(b->part==1){b->address=(uint16_t)value;b->part=2;}
 else if(b->part==2){b->address|=(uint16_t)(value<<8);b->part=3;}
 else {b->regs[b->address++]=(uint8_t)value;b->writes++;}
}
static void write_bus(void*u,uint32_t a,uint32_t value){Bus*b=u;b->ticks++;if(a!=AG_TEXT_BASE+32)return;
 unsigned old=b->levels;b->levels=value;int scl_old=old&1,scl_new=value&1;
 if(scl_old&&scl_new&&(old&2)&&!(value&2)){b->starts++;b->active=1;b->reading=0;b->bits=0;b->byte=0;b->part=0;b->ack=0;b->sda_low=0;b->read_done=0;return;}
 if(scl_old&&scl_new&&!(old&2)&&(value&2)){b->stops++;b->active=0;b->sda_low=0;return;}
 if(!b->active)return;
 if(!scl_old&&scl_new){
  if(b->ack){b->ack_high=1;return;}
  if(b->reading){b->read_bit++;return;}
  b->byte=(b->byte<<1)|!!(value&2);if(++b->bits==8){receive_byte(b,b->byte);b->ack=1;b->ack_high=0;b->bits=0;b->byte=0;}
 }else if(scl_old&&!scl_new){
  if(b->ack){if(b->ack_high){b->ack=0;b->sda_low=0;if(b->reading){b->read_bit=0;b->read_done=0;b->reads++;b->sda_low=!(b->regs[b->address]&128);}}
   else b->sda_low=b->ack_value;return;}
  if(b->reading){if(b->read_bit<8)b->sda_low=!(b->regs[b->address]&(128u>>b->read_bit));else {b->sda_low=0;b->read_done=1;}}
 }
}
static uint64_t clock_bus(void*u){Bus*b=u;return ++b->ticks;}
static int font(void*u,uint32_t c,unsigned char rows[16]){(void)u;(void)c;memset(rows,0,16);return 0;}
static void need(int x,const char*s){if(!x){fprintf(stderr,"HDMI_I2C_FAIL %s\n",s);exit(1);}}
int main(void){Bus b={0};b.levels=3;b.regs[0]=0x10;b.regs[1]=0x72;ag_text_display d;ag_display_io io={&b,read_bus,write_bus,clock_bus,0,1000000};need(!ag_display_init(&d,&io,font,0),"init");ag_hdmi_reset(&d,1);
 need(!ag_hdmi_register_write(&d,0x1234,0xa5)&&b.regs[0x1234]==0xa5,"register address endian/write ACK");
 uint8_t x=0;need(!ag_hdmi_register_read(&d,0x1234,&x)&&x==0xa5,"repeated-start/read data");
 b.nack=1;need(ag_hdmi_register_write(&d,0x1234,0x5a)==-8&&b.regs[0x1234]==0xa5,"NACK handling");b.nack=0;
 b.stuck=1;need(ag_hdmi_register_write(&d,0x1234,0x5a)==-7,"clock stretch timeout");b.stuck=0;b.active=0;b.sda_low=0;
 need(!ag_ms7210_init_480p(&d,1000000),"480p register initialization");
 need(b.regs[0x120c]==0x5a&&b.regs[0x120d]==3&&b.regs[0x120e]==0x0d&&b.regs[0x120f]==2,"720x480 timing totals");
 need((b.regs[0x1200]&12)==4&&(b.regs[0x0503]&1)==1&&(b.regs[0x000b]&0x11)==0x11&&(b.regs[0x0920]&4)==4,"RGB DE mode and output enable");
 printf("HDMI_I2C_PASS starts=%u stops=%u writes=%u reads=%u repeated_start endian nack stretch 480p\n",b.starts,b.stops,b.writes,b.reads);return 0;}
