#include "ag_coremark_ui.h"
#include <string.h>
extern void tcm_benchmark_switch(void);
#define BENCH ((volatile unsigned*)0xbf0d0100u)
static char *put_num(char *d,uint64_t v){char t[24];unsigned n=0;
 if(!v){*d++='0';return d;}
 while(v){t[n++]=(char)('0'+v%10);v/=10;}
 while(n)*d++=t[--n];
 return d;}
static char *put_fix(char *d,uint64_t v,unsigned scale){d=put_num(d,v/scale);*d++='.';
 uint64_t r=v%scale;for(unsigned k=scale/10;k;k/=10)*d++=(char)('0'+(r/k)%10);return d;}
static char *put_milli(char *d,uint64_t v){d=put_num(d,v/1000);*d++='.';d[0]=(char)('0'+v/100%10);d[1]=(char)('0'+v/10%10);d[2]=(char)('0'+v%10);return d+3;}
static char *put_str(char *d,const char *s){while(*s)*d++=*s++;return d;}
static int row(ag_agent_ui*u,unsigned r,unsigned attr,const char*s){return ag_display_text(&u->display,0,r,attr,s,strlen(s));}
int ag_coremark_ui_run(ag_agent_ui*u,uint64_t now){
 (void)now;if(!u||u->error)return -1;
 uint32_t timeout=u->display.io.clock_hz/10u;
 int rc=ag_display_clear(&u->display,timeout);
 if(!rc)rc=row(u,0,15,"RISC-V RV32IM CoreMark v1.0");
 if(!rc)rc=row(u,2,11,"Running in 64 KiB TCM - please wait about 25 s");
 if(!rc)rc=row(u,4,15,"10000 iterations, all three workloads");
 if(!rc)rc=row(u,6,15,"Keys and HDMI updates are paused during the run.");
 if(!rc)rc=row(u,8,15,"Firmware image is saved and restored automatically.");
 if(!rc)rc=ag_display_present(&u->display,timeout);
 if(rc){(void)ag_display_present(&u->display,timeout);u->error=rc;return -1;}
 u->output_bytes=0;u->output[0]=0;u->receipt[0]=0;
 tcm_benchmark_switch();
 unsigned magic=BENCH[0],validated=BENCH[3],failed=BENCH[4],iters=BENCH[9];
 uint32_t exit_code=BENCH[20],trap=BENCH[21];
 uint64_t cycles=((uint64_t)BENCH[2]<<32)|BENCH[1];
 char *d=u->output;char *const end=u->output+sizeof u->output-64;
 int ok=magic==0x434d5231u&&validated&&!failed&&!exit_code&&!trap&&cycles>(uint64_t)CPU_HZ*10u;
 d=put_str(d,ok?"Status: PASS - CRC validated\n":"Status: FAIL - result rejected\n");
 if(magic!=0x434d5231u){d=put_str(d,"Benchmark did not report a result.\n");}
 else{
  d=put_str(d,"Iterations: ");d=put_num(d,iters);*d++='\n';
  d=put_str(d,"Cycles: ");d=put_num(d,cycles);*d++='\n';
  d=put_str(d,"Seconds: ");d=put_fix(d,cycles*1000u/CPU_HZ,1000);*d++='\n';
  if(cycles){uint64_t milli=(uint64_t)iters*CPU_HZ*1000u/cycles;
   d=put_str(d,"CoreMark: ");d=put_milli(d,milli);*d++='\n';
   uint64_t mhz=CPU_HZ/1000000u;
   uint64_t per_mhz=mhz?milli/mhz:0;
   d=put_str(d,"CoreMark/MHz: ");d=put_milli(d,per_mhz);*d++='\n';
  }
  d=put_str(d,"CRC list/matrix/state: ");d=put_num(d,BENCH[6]);d=put_str(d,"/");d=put_num(d,BENCH[7]);
  d=put_str(d,"/");d=put_num(d,BENCH[8]);d=put_str(d,"\n");
 }
 if(exit_code||trap){
  d=put_str(d,"Exit=");d=put_num(d,exit_code);d=put_str(d," Trap=0x");d=put_num(d,trap);*d++='\n';
 }
 d=put_str(d,ok?"K3 rerun   K4 back to menu\n":"See UART output for the full transcript.\n");
 if(d>end)d=end;
 u->output_bytes=(size_t)(d-u->output);u->output[u->output_bytes]=0;
 u->running=0;u->keyboard=0;u->coremark_page=2;u->dirty=1;u->last_draw_ms=0;
 return ok?0:-1;
}
