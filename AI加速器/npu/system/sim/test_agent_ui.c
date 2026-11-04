#include "../sw/ag_agent_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned events,draws,max_row,uart_key;static int display_error;static size_t output_bytes,output_cells;
static uint32_t read_io(void*u,uint32_t address){(void)u;if(address==0x10001210){unsigned e=events;events=0;return e;}if(address==0x10001244)return uart_key!=0;if(address==0x10001240){unsigned key=uart_key;uart_key=0;return key;}return 0;}
int ag_display_init(ag_text_display*d,const ag_display_io*i,ag_glyph_fn g,void*u){memset(d,0,sizeof(*d));d->io=*i;d->glyph=g;d->glyph_user=u;return 0;}
int ag_ms7210_init_480p(ag_text_display*d,uint32_t t){(void)d;(void)t;return 0;}
int ag_display_clear(ag_text_display*d,uint32_t t){(void)d;(void)t;draws++;return display_error;}
int ag_display_present(ag_text_display*d,uint32_t t){(void)d;(void)t;return 0;}
int ag_display_text(ag_text_display*d,unsigned c,unsigned r,unsigned a,const char*s,size_t n){(void)d;(void)c;(void)a;if(r>max_row)max_row=r;if(r>=5&&r<26){output_bytes+=n;for(size_t i=0;i<n;i++)if(((unsigned char)s[i]&0xc0)!=0x80)output_cells++;}if(n&&((unsigned char)s[n-1]&0xc0)==0xc0)return -6;return 0;}
const char*ag_loop_status_name(ag_loop_status s){return s==AG_LOOP_RUNNING?"RUNNING":"FINISHED";}
static int glyph(void*u,uint32_t cp,unsigned char rows[16]){(void)u;(void)cp;memset(rows,0,16);return 0;}
static uint64_t clock_io(void*u){(void)u;return 1000;}
static void need(int x,const char*s){if(!x){fprintf(stderr,"AGENT_UI_FAIL %s\n",s);exit(1);}}
int main(void){ag_agent_ui u;ag_display_io io={0,read_io,0,clock_io,0,102000000};ag_loop loop;memset(&loop,0,sizeof loop);loop.status=AG_LOOP_RUNNING;
 need(!ag_agent_ui_init(&u,&io,glyph,0),"init");need(strstr(ag_agent_ui_task(&u),"12+8")!=0,"first task");
 events=1u<<2;need(!ag_agent_ui_poll(&u,0,10)&&u.selected==1,"next");events=1u<<1;ag_agent_ui_poll(&u,0,20);need(u.selected==0,"previous");events=1u<<1;ag_agent_ui_poll(&u,0,30);need(u.selected==4,"wrap");events=1u<<3;need(ag_agent_ui_poll(&u,0,40)==0&&u.coremark_page==1,"CoreMark first press only opens warning");events=16;need(ag_agent_ui_poll(&u,0,41)==0&&!u.coremark_page,"CoreMark warning back");events=8;need(!ag_agent_ui_poll(&u,0,42),"CoreMark warning reopen");events=8;need(ag_agent_ui_poll(&u,0,43)==2,"CoreMark explicit second press starts");u.coremark_page=0;u.selected=2;
 uart_key='w';ag_agent_ui_poll(&u,0,50);need(u.selected==1,"UART previous");uart_key='s';ag_agent_ui_poll(&u,0,60);need(u.selected==2,"UART next");uart_key=13;need(ag_agent_ui_poll(&u,0,70)==1,"UART enter");
 ag_agent_ui_begin(&u);
 ag_agent_ui_piece(&u,"hello",5);ag_agent_ui_piece(&u,"\xe4\xb8",2);need(!ag_agent_ui_poll(&u,&loop,1000),"partial UTF8");need(output_bytes==5,"partial codepoint displayed");
 ag_agent_ui_piece(&u,"\xad",1);output_bytes=0;ag_agent_ui_poll(&u,&loop,1400);need(output_bytes==8,"complete UTF8 lost");
 events=1u<<4;need(ag_agent_ui_poll(&u,&loop,1500)==-1,"cancel");uart_key=27;need(ag_agent_ui_poll(&u,&loop,1550)==-1,"UART cancel");u.escape_state=0;
 char long_text[2048];memset(long_text,'A',sizeof long_text);ag_agent_ui_piece(&u,long_text,sizeof long_text);output_bytes=0;output_cells=0;ag_agent_ui_poll(&u,&loop,2000);need(u.output_bytes<sizeof u.output&&output_cells<=1680&&max_row<30,"bounded transcript");
 display_error=-2;ag_agent_ui_stage(&u,"failure");ag_agent_ui_poll(&u,&loop,2500);need(u.error==-2,"display error");events=1u<<4;need(ag_agent_ui_poll(&u,&loop,2600)==-1,"cancel lost after display failure");
 loop.status=AG_LOOP_FINISHED;ag_agent_ui_end(&u,&loop);need(!u.running,"finish");
 u.error=0;display_error=0;u.selected=3;events=8;need(!ag_agent_ui_poll(&u,0,2800)&&u.keyboard,"open keyboard");
 events=8;ag_agent_ui_poll(&u,0,2900);need(!strcmp(u.input,"a"),"virtual a");
 events=4;ag_agent_ui_poll(&u,0,3000);events=8;ag_agent_ui_poll(&u,0,3100);need(!strcmp(u.input,"ab"),"virtual b");
 events=16u<<5;ag_agent_ui_poll(&u,0,3200);need(!strcmp(u.input,"a"),"release backspace");
 uart_key='w';ag_agent_ui_poll(&u,0,3300);uart_key='q';ag_agent_ui_poll(&u,0,3400);need(!strcmp(u.input,"awq"),"UART letters not commands while typing");
 uart_key=13;need(ag_agent_ui_poll(&u,0,3500)==1&&!strcmp(ag_agent_ui_task(&u),"awq"),"typed prompt submit");
 u.keyboard=1;u.input_bytes=0;u.input[0]=0;events=16u<<10;need(!ag_agent_ui_poll(&u,0,3600),"empty submit blocked");
 uart_key='X';ag_agent_ui_poll(&u,0,3700);events=16u<<10;need(ag_agent_ui_poll(&u,0,3800)==1,"long key submit");printf("AGENT_UI_PASS draws=%u utf8 task_selection real_callbacks cancel_display_error bounded_output\n",draws);return 0;}
