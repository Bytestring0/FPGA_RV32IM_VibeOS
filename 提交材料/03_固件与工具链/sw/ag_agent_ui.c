#include "ag_agent_ui.h"
#include <string.h>
static const char *const tasks[]={"Calculate (12+8)*3 and tell me the result.","Query the note count and timer status.","Start a 10 second timer named Demo.","Direct chat (no tools)","CoreMark / Run benchmark"};
static const char keys[]="abcdefghijklmnopqrstuvwxyz0123456789 +-*/().,?!:_=";
#define CHAR_KEYS (sizeof keys-1u)
#define KEY_COUNT (CHAR_KEYS+5u)
static void copy_text(char*d,size_t cap,const char*s){size_t n=0;if(s)while(s[n]&&n+1<cap){d[n]=s[n];n++;}d[n]=0;}
static int line(ag_agent_ui*u,unsigned row,unsigned attr,const char*s){return ag_display_text(&u->display,0,row,attr,s,strlen(s));}
static void append(ag_agent_ui*u,unsigned c){if(u->input_bytes+1<sizeof u->input){u->input[u->input_bytes++]=(char)c;u->input[u->input_bytes]=0;u->dirty=1;}}
static void backspace(ag_agent_ui*u){if(u->input_bytes){do{u->input_bytes--;}while(u->input_bytes&&((unsigned char)u->input[u->input_bytes]&0xc0)==0x80);u->input[u->input_bytes]=0;u->dirty=1;}}
static int submit(ag_agent_ui*u){if(!u->input_bytes)return 0;u->selected=3;u->keyboard=0;u->dirty=1;return 1;}
int ag_agent_ui_init(ag_agent_ui*u,const ag_display_io*io,ag_glyph_fn glyph,void*font){if(!u)return -1;memset(u,0,sizeof *u);int rc=ag_display_init(&u->display,io,glyph,font);if(!rc)rc=ag_ms7210_init_480p(&u->display,io->clock_hz);if(rc)return u->error=rc;copy_text(u->stage,sizeof u->stage,"Preparing model");u->dirty=1;return 0;}
void ag_agent_ui_stage(ag_agent_ui*u,const char*s){if(u){copy_text(u->stage,sizeof u->stage,s);u->dirty=1;}}
void ag_agent_ui_piece(ag_agent_ui*u,const char*s,size_t n){if(!u||!s)return;if(n>=sizeof u->output){s+=n-(sizeof u->output-1);n=sizeof u->output-1;while(n&&((unsigned char)*s&0xc0)==0x80){s++;n--;}}if(n>sizeof u->output-1-u->output_bytes){size_t drop=n-(sizeof u->output-1-u->output_bytes);while(drop<u->output_bytes&&((unsigned char)u->output[drop]&0xc0)==0x80)drop++;for(size_t i=drop;i<u->output_bytes;i++)u->output[i-drop]=u->output[i];u->output_bytes-=drop;}memcpy(u->output+u->output_bytes,s,n);u->output_bytes+=n;u->output[u->output_bytes]=0;u->generation_steps++;u->dirty=1;}
void ag_agent_ui_receipt(ag_agent_ui*u,const char*s){if(u){copy_text(u->receipt,sizeof u->receipt,s);u->dirty=1;}}
const char*ag_agent_ui_task(const ag_agent_ui*u){return u&&u->selected==3?u->input:tasks[u&&u->selected<5?u->selected:0];}
void ag_agent_ui_trace(ag_agent_ui*u){if(u)u->dirty=1;}
void ag_agent_ui_begin(ag_agent_ui*u){if(u){u->running=1;u->coremark_page=0;u->generation_steps=0;u->output_bytes=0;u->output[0]=u->receipt[0]=0;ag_agent_ui_stage(u,"NPU running / K4 or Esc to cancel");}}
void ag_agent_ui_end(ag_agent_ui*u,const ag_loop*l){if(u){u->running=0;ag_agent_ui_stage(u,l?ag_loop_status_name(l->status):"Stopped");}}
static size_t prefix(const char*s,size_t n,unsigned limit){size_t p=0;unsigned count=0;while(p<n&&count<limit){unsigned b=(unsigned char)s[p],len=b<128?1:b>=0xc2&&b<0xe0?2:b<0xf0?3:4;if(p+len>n)break;if(b=='\n'||b=='\r')break;unsigned j;for(j=1;j<len;j++)if(((unsigned char)s[p+j]&0xc0)!=0x80)break;if(j!=len)break;p+=len;count++;}return p;}
static int wrapped(ag_agent_ui*u,unsigned first,unsigned last,unsigned attr,const char*s,size_t n){size_t at=0;int rc=0;for(unsigned row=first;row<last&&at<n&&!rc;row++){size_t take=prefix(s+at,n-at,80);if(take)rc=ag_display_text(&u->display,0,row,attr,s+at,take);at+=take;if(at<n&&(s[at]=='\n'||s[at]=='\r')){char c=s[at++];if(c=='\r'&&at<n&&s[at]=='\n')at++;}else if(!take)break;}return rc;}
static int choose_key(ag_agent_ui*u){unsigned k=u->key_index;if(k<CHAR_KEYS){unsigned c=(unsigned char)keys[k];if(u->uppercase&&c>='a'&&c<='z')c-=32;append(u,c);}else if(k==CHAR_KEYS)backspace(u);else if(k==CHAR_KEYS+1)u->uppercase=!u->uppercase;else if(k==CHAR_KEYS+2){u->input_bytes=0;u->input[0]=0;}else if(k==CHAR_KEYS+3)return submit(u);else u->keyboard=0;u->dirty=1;return 0;}
void ag_agent_ui_progress(ag_agent_ui*u,const char*stage,unsigned permille){
 if(!u||u->error||permille>1000u)return;
 uint32_t timeout=u->display.io.clock_hz/10u;int rc=ag_display_clear(&u->display,timeout);
 char bar[24];unsigned fill=permille*20u/1000u;
 for(unsigned i=0;i<20;i++)bar[i]=i<fill?'#':'-';bar[20]=0;
 char pct[8];unsigned p=permille/10u;pct[0]=(char)('0'+p/100u);pct[1]=(char)('0'+(p/10u)%10u);pct[2]=(char)('0'+p%10u);copy_text(pct+3,sizeof pct-3,"%");
 if(!rc)rc=line(u,0,15,"RISC-V + NPU  |  MODEL LOAD");
 if(!rc)rc=line(u,3,14,stage);
 if(!rc)rc=line(u,7,15,bar);
 if(!rc)rc=line(u,8,11,pct);
 if(!rc)rc=line(u,11,7,"Progress counts real bytes read from the SD card.");
 if(!rc)rc=line(u,12,7,"This screen updates about once per percent.");
 if(!rc)rc=ag_display_present(&u->display,timeout);
 if(rc){(void)ag_display_present(&u->display,timeout);u->error=rc;}
}

int ag_agent_ui_poll(ag_agent_ui*u,const ag_loop*l,uint64_t now){
 if(!u)return 0;uint32_t e=u->display.io.read(u->display.io.user,0x10001210u),press=e&31u;int action=0;
#ifdef AGENT_UART_ASSETS
 if(u->display.io.read(u->display.io.user,0x10001244u)&1u){unsigned key=u->display.io.read(u->display.io.user,0x10001240u)&255u;
  if(u->running){if(key==27||key==3)action=-1;}
  else if(key==27){u->keyboard=0;u->escape_state=1;u->dirty=1;}
  else if(u->escape_state){if(key=='[')u->escape_state=2;else{if(u->escape_state==2){if(key=='D'||key=='A')press|=2;if(key=='C'||key=='B')press|=4;}u->escape_state=0;}}
  else if(u->keyboard){if(key==13){action=submit(u);u->ignore_lf=1;}else if(key==10){if(!u->ignore_lf)action=submit(u);u->ignore_lf=0;}else if(key==8||key==127)backspace(u);else if(key==9)u->key_index=(u->key_index+1)%KEY_COUNT;else if(key>=32&&key<127){u->ignore_lf=0;append(u,key);}}
  else if(key=='w')press|=2;else if(key=='s')press|=4;else if(key==13||key==10)press|=8;else if(key=='q')press|=16;
  else if(key>=32&&key<127){u->selected=3;u->keyboard=1;u->coremark_page=0;u->input_bytes=0;append(u,key);}
 }
#endif
 if(u->running){if(press&16)action=-1;}
 else if(!action&&u->keyboard){if(press&2){u->key_index=(u->key_index+KEY_COUNT-1)%KEY_COUNT;u->dirty=1;}if(press&4){u->key_index=(u->key_index+1)%KEY_COUNT;u->dirty=1;}if(press&8)action=choose_key(u);if(e&(16u<<10))action=submit(u);else if(e&(16u<<5))backspace(u);}
 else if(!action){if(press&2){u->selected=(u->selected+4)%5;u->coremark_page=0;u->dirty=1;}if(press&4){u->selected=(u->selected+1)%5;u->coremark_page=0;u->dirty=1;}if(press&8){if(u->selected==3){u->keyboard=1;u->coremark_page=0;u->key_index=0;u->dirty=1;}else if(u->selected==4){if(u->coremark_page==1){u->coremark_page=2;action=2;}else{u->coremark_page=1;u->output_bytes=0;u->output[0]=0;}u->dirty=1;}else action=1;}}
 if(!u->running&&!u->keyboard&&(press&16)){u->coremark_page=0;u->dirty=1;}
 if(u->error||!u->dirty||now-u->last_draw_ms<150)return action;
 u->last_draw_ms=now;uint32_t timeout=u->display.io.clock_hz/10u;int rc=ag_display_clear(&u->display,timeout);
 if(!rc)rc=line(u,0,15,"RISC-V + NPU  |  CHAT / AGENT / COREMARK");
 if(!rc)rc=line(u,1,7,"----------------------------------------------------------------------");
 if(!rc&&u->keyboard){
  rc=line(u,2,14,"TYPE A MESSAGE  (UART keyboard: Enter sends)");
  if(!rc)rc=line(u,3,7,"K1/K2 move cursor  K3 type  K4 delete  BACKSPACE key deletes");
  if(!rc)rc=wrapped(u,5,8,15,u->input,u->input_bytes);
  for(unsigned k=0;k<KEY_COUNT&&!rc;k++){unsigned col=(k%8)*10,row=9+k/8;char label[10]={' ',0};
   if(k<CHAR_KEYS){label[1]=keys[k];if(u->uppercase&&label[1]>='a'&&label[1]<='z')label[1]-=32;
    if(keys[k]==' ')copy_text(label,sizeof label," Space ");else{label[2]=' ';label[3]=0;}}
   else copy_text(label,sizeof label,k==CHAR_KEYS?" DEL ":k==CHAR_KEYS+1?" Shift ":k==CHAR_KEYS+2?" Clear ":k==CHAR_KEYS+3?" SEND ":" Back ");
   rc=ag_display_text(&u->display,col,row,k==u->key_index?0xf0:0x0f,label,strlen(label));}
 }
 else if(!rc&&u->coremark_page==1){
  rc=line(u,2,15,"COREMARK - READY TO START");
  if(!rc)rc=line(u,5,15,"Runs 10000 iterations entirely inside the 64 KiB TCM.");
  if(!rc)rc=line(u,6,15,"Measurement takes about 25 seconds and is not interrupted.");
  if(!rc)rc=line(u,7,15,"The current firmware image is saved and restored.");
  if(!rc)rc=line(u,8,15,"Result shows cycles, CoreMark, CoreMark/MHz and CRC state.");
  if(!rc)rc=line(u,10,7,"Press K4 to go back without running anything.");
  if(!rc)rc=line(u,12,0xf0,"K3 / Enter: START");
 }
 else if(!rc&&(u->running||u->coremark_page==2||u->output_bytes)){
  rc=line(u,2,11,u->stage);
  if(!rc&&l&&u->running)rc=line(u,3,10,ag_loop_status_name(l->status));
  if(!rc)rc=wrapped(u,5,26,15,u->output,u->output_bytes);
  if(!rc&&u->receipt[0]){rc=line(u,27,14,"REAL TOOL RECEIPT");if(!rc)rc=wrapped(u,28,30,7,u->receipt,strlen(u->receipt));}
 }
 else if(!rc){
  rc=line(u,2,11,u->stage);
  for(unsigned i=0;i<5&&!rc;i++)rc=line(u,5+i*2,i==u->selected?0xf0:0x0f,tasks[i]);
  if(!rc)rc=line(u,16,15,"Nothing runs automatically at startup.");
  if(!rc)rc=line(u,18,15,"K1/K2 choose   K3 select   K4 back   K0 reset");
  if(!rc)rc=line(u,19,7,"UART: w/s move, Enter select. Or type a letter to chat.");
 }
 if(!rc)rc=ag_display_present(&u->display,timeout);if(rc){(void)ag_display_present(&u->display,timeout);u->error=rc;}else u->dirty=0;return action;
}
