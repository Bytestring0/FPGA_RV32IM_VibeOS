#include "platform.h"
#include <stdarg.h>
static volatile unsigned *const result=(volatile unsigned*)0xbf0d0100u;
static int slot;
static int has(const char*s,const char*t){for(;*s;s++){const char*a=s,*b=t;while(*b&&*a==*b){a++;b++;}if(!*b)return 1;}return 0;}
static void note(const char*fmt){slot=-1;
 if(has(fmt,"seedcrc"))slot=5;else if(has(fmt,"crclist"))slot=6;else if(has(fmt,"crcmatrix"))slot=7;else if(has(fmt,"crcstate"))slot=8;
 if(has(fmt,"Correct operation validated"))result[3]=1;
 if(has(fmt,"Errors detected")||has(fmt,"ERROR!")||has(fmt,"Cannot validate"))result[4]=1;}
static int out_uint(unsigned n,unsigned base,unsigned width,char pad){char b[32];unsigned len=0;int c=0;
 do{b[len++]="0123456789abcdef"[n%base];n/=base;}while(n);
 while(width>len){putchar_uart(pad);width--;c++;}
 while(len){putchar_uart(b[--len]);c++;}return c;}
int ee_printf(const char*fmt,...){note(fmt);va_list ap;va_start(ap,fmt);int c=0;
 while(*fmt){if(*fmt!='%'){putchar_uart(*fmt++);c++;continue;}fmt++;
  char pad=' ';unsigned width=0;
  if(*fmt=='0'){pad='0';fmt++;}
  while(*fmt>='0'&&*fmt<='9'){width=width*10+(unsigned)(*fmt++-'0');}
  if(*fmt=='l')fmt++;
  switch(*fmt++){
   case 's':{const char*s=va_arg(ap,const char*);while(*s){putchar_uart(*s++);c++;}break;}
   case 'd':{int v=va_arg(ap,int);if(v<0){putchar_uart('-');c++;}c+=out_uint(v<0?0u-(unsigned)v:(unsigned)v,10,width,pad);break;}
   case 'u':c+=out_uint(va_arg(ap,unsigned),10,width,pad);break;
   case 'x':{unsigned v=va_arg(ap,unsigned);if(slot>=0){result[slot]=v;slot=-1;}c+=out_uint(v,16,width,pad);break;}
   case 'c':putchar_uart((char)va_arg(ap,int));c++;break;
   case '%':putchar_uart('%');c++;break;
   default:putchar_uart('?');c++;break;}
 }
 va_end(ap);return c;}
