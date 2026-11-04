#include "coremark.h"
#include "platform.h"
volatile ee_s32 seed1_volatile=0,seed2_volatile=0,seed3_volatile=0x66,seed4_volatile=ITERATIONS,seed5_volatile=0;
ee_u32 default_num_contexts=1;
static CORE_TICKS begin,end;
static volatile unsigned *const result=(volatile unsigned*)0xbf0d0100u;
void start_time(void){begin=read_cycles();}
void stop_time(void){end=read_cycles();}
CORE_TICKS get_time(void){return end-begin;}
secs_ret time_in_secs(CORE_TICKS t){return (secs_ret)(t/CPU_HZ);}
void portable_init(core_portable*p,int*argc,char*argv[]){(void)argc;(void)argv;
 for(unsigned i=0;i<24;i++)result[i]=0;
 result[0]=0x434d5231u;result[9]=(unsigned)ITERATIONS;p->portable_id=1;}
void portable_fini(core_portable*p){CORE_TICKS t=get_time();
 result[1]=(unsigned)t;result[2]=(unsigned)(t>>32);
 puts_uart("MEASURED_CYCLES_HEX=");puthex((unsigned)(t>>32));puthex((unsigned)t);puts_uart("\n");
 p->portable_id=0;}
