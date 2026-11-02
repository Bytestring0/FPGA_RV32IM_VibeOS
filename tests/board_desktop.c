/* VibeOS task desktop firmware. All app state is volatile and shared by the
 * deterministic tools and the explicitly SCRIPTED assistant adapter. */
#include "../sw/platform.h"
#include "../sw/drivers/terminal.h"
#include "../sw/drivers/button.h"
#include "../sw/agent/ag_agent.h"
#include "../sw/agent/ag_loop.h"
#include "../sw/text/han_ime.h"
#ifdef DESKTOP_PC_QWEN
#include "../sw/agent/desktop_pc_bridge.h"
#define BACKEND_NAME "PC QWEN"
#define TASK_TIMEOUT_MS PC_BRIDGE_TIMEOUT_MS
#else
#define BACKEND_NAME "SCRIPTED"
#define TASK_TIMEOUT_MS 15000u
#endif

#define COLS 80u
#define ROWS 30u
#define INPUT_CAP 120u
#define LOG_CAP 12u
#define LOG_TEXT 48u
#define KEYSET "qwertyuiopasdfghjklzxcvbnm0123456789 +-*/().:?"
#define KEY_COUNT (sizeof(KEYSET)-1u)

void *memset(void *dst,int value,size_t count){volatile unsigned char*p=(volatile unsigned char*)dst;void*out=dst;while(count--)*p++=(unsigned char)value;return out;}
void *memcpy(void *dst,const void *src,size_t count){volatile unsigned char*d=(volatile unsigned char*)dst;const volatile unsigned char*s=(const volatile unsigned char*)src;void*out=dst;while(count--)*d++=*s++;return out;}

#ifndef DESKTOP_STEP_CYCLES
#define DESKTOP_STEP_CYCLES 2700000ull
#endif

static ag_loop app;
static unsigned app_ready;
static unsigned page, layer, focus, subpage;
static unsigned editor, chinese, key_focus, cand_focus, cand_count, candidate_mode;
static char input[INPUT_CAP+1u], pinyin[32];
static unsigned input_len, pinyin_len;
static char status_text[80]="READY";
static char last_event[80]="No task yet";
static char last_result[AG_RESULT_MAX+1];
static unsigned task_id, direct_id, log_count, log_start;
static struct { char source[9], task[8], tool[20], result[LOG_TEXT]; } logs[LOG_CAP];
static uint64_t last_step_cycles;
// Edge latches can report press+long+release in one poll after a frame commit.
// Suppress the short action whenever that poll contains a long action.
static unsigned k4_armed, k2_armed, k0_armed, k1_armed;
static unsigned error_visible;

static void cp(char *d,const char*s,unsigned cap){unsigned i=0;if(!cap)return;while(i+1<cap&&s&&s[i]){d[i]=s[i];i++;}d[i]=0;}
static unsigned slen(const char*s){unsigned n=0;while(s&&s[n])n++;return n;}
static void cat(char*d,const char*s,unsigned cap){unsigned n=slen(d),i=0;if(n>=cap)return;while(n+1<cap&&s&&s[i])d[n++]=s[i++];d[n]=0;}
static void num(char*out,unsigned v){char t[10];unsigned n=0;do{t[n++]=(char)('0'+v%10);v/=10;}while(v);while(n)*out++=t[--n];*out=0;}
static uint64_t now_ms(void){
#ifdef DESKTOP_PC_QWEN
return pc_bridge_mailbox.wall_ms;
#else
return read_cycles()/27000u;
#endif
}
static const char *pages[]={"ASSISTANT","TOOLS","RECORDS","RUN LOG"};
static const char *timer_name(ag_timer_status s){return s==AG_RUNNING?"RUN":s==AG_PAUSED?"PAUSE":s==AG_FINISHED?"DONE":"IDLE";}
/* All text owns a bounded cell rectangle: terminal writes auto-increment and
 * must never be allowed to spill into an adjacent row or navigation field. */
static unsigned render_glyph(const char *s,unsigned *bytes,unsigned *width){
 unsigned char c=(unsigned char)s[0];uint32_t code;unsigned g;
 *bytes=1;*width=1;
 if(c<0x80)return c>=32&&c<127?c:'?';
 if(c>=0xc2&&c<=0xdf&&s[1]&&((unsigned char)s[1]&0xc0)==0x80){*bytes=2;return '?';}
 if(c>=0xe0&&c<=0xef&&s[1]&&s[2]&&((unsigned char)s[1]&0xc0)==0x80&&((unsigned char)s[2]&0xc0)==0x80){
  code=((c&15u)<<12)|(((unsigned char)s[1]&63u)<<6)|((unsigned char)s[2]&63u);
  if(code<0x800u||(code>=0xd800u&&code<=0xdfffu))return '?';
  *bytes=3;
  g=code==0x8ba1?0:code==0x7b97?1:code==0x4fdd?2:code==0x5b58?3:code==0x67e5?4:code==0x8be2?5:code==0x8bbe?6:code==0x7f6e?7:code==0x5206?8:code==0x949f?9:code==0x65f6?10:code==0x5668?11:code==0x6682?12:code==0x505c?13:code==0x7ee7?14:code==0x7eed?15:code==0x53d6?16:code==0x6d88?17:code==0x5ef6?18:code==0x957f?19:code==0x7b14?20:code==0x8bb0?21:code==0x79d2?22:99;
  if(g<23){*width=2;return 0x80u+2u*g;}
  {int ix=han_glyph_index(code);if(ix>=0){*width=2;return 0x10000u+2u*(unsigned)ix;}}return '?';
 }
 if(c>=0xf0&&c<=0xf4&&s[1]&&s[2]&&s[3]&&((unsigned char)s[1]&0xc0)==0x80&&((unsigned char)s[2]&0xc0)==0x80&&((unsigned char)s[3]&0xc0)==0x80){
  code=((c&7u)<<18)|(((unsigned char)s[1]&63u)<<12)|(((unsigned char)s[2]&63u)<<6)|((unsigned char)s[3]&63u);
  if(code>=0x10000u&&code<=0x10ffffu)*bytes=4;
 }
 return '?';
}
static void text_box(uint32_t row,uint32_t col,unsigned width,unsigned height,const char*s,unsigned attr){
 unsigned x=0,y=0;if(!s||row>=ROWS||col>=COLS||!width||!height)return;
 if(width>COLS-col)width=COLS-col;
 if(height>ROWS-row)height=ROWS-row;
 term_gotoxy(row,col);
 while(*s&&y<height){
  unsigned bytes,cells,glyph;
  if(*s=='\r'){s++;continue;}
  if(*s=='\n'){s++;x=0;if(++y>=height)break;term_gotoxy(row+y,col);continue;}
  glyph=render_glyph(s,&bytes,&cells);
  if(cells>width)break;
  if(x+cells>width){x=0;if(++y>=height)break;term_gotoxy(row+y,col);}
  term_putc_attr(glyph,attr);if(cells==2)term_putc_attr(glyph+1,attr);
  x+=cells;s+=bytes;
 }
}
static void put(uint32_t r,uint32_t c,const char*s){text_box(r,c,c<COLS?COLS-c:0,1,s,0);}
static void inv(uint32_t r,uint32_t c,const char*s){text_box(r,c,c<COLS?COLS-c:0,1,s,1);}
static void field(uint32_t r,uint32_t c,unsigned width,const char*s,unsigned attr){
 if(r>=ROWS||c>=COLS)return;
 if(width>COLS-c)width=COLS-c;
 term_gotoxy(r,c);for(unsigned i=0;i<width;i++)term_putc_attr(' ',attr);
 text_box(r,c,width,1,s,attr);
}
static void line(uint32_t r){if(r>=ROWS)return;term_gotoxy(r,0);for(unsigned i=0;i<COLS;i++)term_putc('-');}
static const char *render_backend(void){
#ifdef DESKTOP_PC_QWEN
 return "PC QWEN";
#else
 return "SCRIPTED";
#endif
}
static void clear_receipts(void){app.runtime.state.record_count=0;for(unsigned i=0;i<AG_RECEIPTS_MAX;i++){app.runtime.state.records[i].id[0]=0;app.runtime.state.records[i].request[0]=0;app.runtime.state.records[i].tool[0]=0;}}
static void log_event(const char*source,const char*tid,const char*tool,const char*result){unsigned at;if(log_count<LOG_CAP){at=(log_start+log_count++)%LOG_CAP;}else{at=log_start;log_start=(log_start+1)%LOG_CAP;}cp(logs[at].source,source,sizeof logs[at].source);cp(logs[at].task,tid,sizeof logs[at].task);cp(logs[at].tool,tool,sizeof logs[at].tool);cp(logs[at].result,result,sizeof logs[at].result);}
static void task_text(char*out,unsigned n){out[0]='T';for(unsigned i=0;i<6;i++){out[6-i]=(char)('0'+n%10);n/=10;}out[7]=0;}
static void direct_tool(const char*tool,const char*json){char id[AG_ID_MAX+1],tid[8];ag_receipt receipt;clear_receipts();task_text(tid,++task_id);direct_id++;id[0]='D';num(id+1,direct_id);ag_code rc=ag_execute(&app.runtime,id,json,now_ms(),&receipt);cp(last_result,receipt.result,sizeof last_result);cp(last_event,rc==AG_OK?receipt.result:ag_code_name(rc),sizeof last_event);error_visible=rc!=AG_OK;cp(status_text,rc==AG_OK?"DIRECT OK":"DIRECT ERROR",sizeof status_text);log_event("DIRECT",tid,tool,rc==AG_OK?receipt.result:ag_code_name(rc));clear_receipts();}
static unsigned contains(const char*a,const char*b){unsigned na=slen(a),nb=slen(b);if(!nb)return 1;for(unsigned i=0;i+nb<=na;i++){unsigned j=0;while(j<nb&&a[i+j]==b[j])j++;if(j==nb)return 1;}return 0;}

/* Fixed vocabulary only; compatibility marker for renderer extraction.
 * The implementation now uses the external ROM-backed common-character index. */
static void rebuild_candidates(void){
 cand_count=han_ime_find(pinyin);
 cand_focus=cand_count?(cand_focus<cand_count?cand_focus:0):0;
}
static const char *candidate(unsigned wanted){return han_ime_candidate(wanted);}
static void utf8_backspace(char*s,unsigned*len){if(!*len)return;unsigned i=*len-1;while(i>0&&((unsigned char)s[i]&0xc0)==0x80)i--;s[i]=0;*len=i;}
static int append(const char*s){unsigned n=slen(s);if(n>INPUT_CAP-input_len)return 0;for(unsigned i=0;i<n;i++)input[input_len++]=s[i];input[input_len]=0;return 1;}
static void emit_utf8(uint32_t row,uint32_t col,const char*s){text_box(row,col,col<COLS?COLS-col:0,1,s,0);}
static void put_utf(uint32_t r,uint32_t c,const char*s){emit_utf8(r,c,s);}
static void title(void){
 char up[20],left[20];uint64_t now=now_ms();
 uint64_t rem=app.runtime.state.timer==AG_RUNNING?(app.runtime.state.deadline_ms>now?app.runtime.state.deadline_ms-now:0):app.runtime.state.remaining_ms;
 field(0,1,22,"VibeOS TASK DESKTOP",0);field(0,25,12,render_backend(),0);
 field(0,39,12,"VOLATILE",0);put(0,63,"UP");num(up,(unsigned)(now/1000));field(0,66,10,up,0);put(0,77,"s");
 line(1);
 /* Navigation ends at column 53; countdown exclusively owns 55..78. */
 for(unsigned p=0;p<4;p++)field(2,1+p*13,12,pages[p],p==page&&layer==0);
 put(2,55,"TMR");field(2,59,5,timer_name(app.runtime.state.timer),0);
 num(left,(unsigned)(rem/1000));field(2,66,10,left,0);put(2,77,"s");line(3);
 if(error_visible){inv(4,1,"ERROR:");text_box(4,8,70,1,status_text,1);}
 else text_box(4,1,78,1,status_text,0);
}
static void footer(const char*s){
 line(26);
 if(layer==2&&!editor){
  field(27,1,16,"(INPUT TASK)",focus==0);field(27,19,16,"(QUICK CALC)",focus==1);field(27,37,12,"(RETURN)",focus==2);
 }else text_box(27,1,78,1,s,0);
 if(editor){
  put(28,1,candidate_mode&&cand_count?"K0 PREV / K1 NEXT  K2 COMMIT  HOLD K2 COMMIT  HOLD K3 CLEAR":"K0 PREV / K1 NEXT  K2 TYPE/COMMIT  HOLD K2 EN/ZH  HOLD K3 CLEAR");
  put(29,1,candidate_mode?"FOCUS: CANDIDATE  K3 KEYBOARD  K4 BACK  HOLD K4 SEND":"FOCUS: KEYBOARD  K4 BACK  HOLD K4 SEND");
 }else{
#ifdef DESKTOP_PC_QWEN
  if(app.status==AG_LOOP_RUNNING)put(28,1,"K0 PREV / K1 NEXT  K2 SELECT  K3 BACK  K4 CANCEL REQUEST");
  else
#endif
  put(28,1,"K0 PREV / K1 NEXT  K2 SELECT  K3 BACK  K4 BACK  HOLD K4 ACTIONS");
  put(29,1,layer==0?"FOCUS: NAVIGATION  K2 ENTER PAGE":layer==2?"FOCUS: BOTTOM ACTIONS  K2 RUN  K3 RETURN":"FOCUS: PAGE CONTENT  K3 RETURN TO NAVIGATION");
 }
}
static void draw_editor(void){
 const char*heading=page==1&&focus==2?"SET TIMER MINUTES":page==1&&focus==4?"EXTEND TIMER MINUTES":page==1&&focus==6?"ADD NOTE":page==0?"EDIT ASSISTANT TASK":"EDIT CALCULATION";
 put(5,1,heading);put(6,1,"TEXT:");text_box(6,7,71,2,input,0);
 put(9,1,chinese?"ZH / PINYIN:":"EN / QWERTY:");field(9,15,20,pinyin,0);
 if(cand_count){
  put(10,1,candidate_mode?"CANDIDATES: K0/K1 MOVE / K2 COMMIT / K3 KEYBOARD":"K3 ENTER CANDIDATES. K2 CONTINUES PINYIN");
  unsigned x=2;
  unsigned first=(cand_focus/5u)*5u; char page_num[12];num(page_num,cand_focus/5u+1u);put(12,1,"PAGE");put(12,6,page_num);put(12,12,"K0/K1 MOVE; HOLD K0/K1 PAGE; K2 CONFIRM");
  for(unsigned i=first;i<cand_count&&i<first+5u;i++){
   char ix[5];num(ix,i-first+1);put(11,x,ix);text_box(11,x+2,12,1,candidate(i),i==cand_focus&&candidate_mode);x+=15;
  }
 }else put(10,1,chinese?"TYPE PINYIN. K3 DELETE. K2 ADDS FOCUSED KEY":"K2 ADDS FOCUSED KEY. K3 DELETES LAST CHARACTER");
 put(13,1,"FULL KEYBOARD  K0/K1 MOVE. HOLD K0 DELETE. CANDIDATES AUTO-PAGE");
 /* KEYSET indices stay unchanged: letters 0..25, digits 26..35,
  * space 36, operators 37..45. Layout only; no synthetic key entries. */
 for(unsigned i=0;i<KEY_COUNT;i++){
  unsigned r,c;char one[4]={'(',KEYSET[i],')',0};
  if(i<10){r=14;c=4+i*5;}
  else if(i<19){r=15;c=6+(i-10)*5;}
  else if(i<26){r=16;c=10+(i-19)*5;}
  else if(i<36){r=18;c=4+(i-26)*5;}
  else if(i==36){r=20;c=4;}
  else{r=20;c=14+(i-37)*5;}
  if(i==36)field(r,c,7,"(SPACE)",i==key_focus&&!candidate_mode);
  else field(r,c,3,one,i==key_focus&&!candidate_mode);
 }
 put(22,1,"(K2 TYPE/COMMIT)  (K3 DELETE / CANDIDATES)  (HOLD K3 CLEAR)");
 put(23,1,candidate_mode&&cand_count?"(HOLD K2 COMMIT)  (K4 BACK)                (HOLD K4 SEND)":"(HOLD K2 EN/ZH)   (K4 BACK)                (HOLD K4 SEND)");
 put(24,1,"Action entries use the named physical button. K0/K1 selects keys.");
 footer(chinese?"ZH: 3755 COMMON HANZI + WORDS. K3 CANDIDATES. HOLD K0 DELETE":"EN: LETTERS / DIGITS / SPACE / OPERATORS. HOLD K4 TO SUBMIT");
}
static void draw_assistant(void){
 if(editor){draw_editor();return;}
 put(5,2,"ASSISTANT TASK ADAPTER");
 field(7,4,42,"Input task",layer==1&&focus==0);put(7,48,render_backend());
 field(8,4,42,"Quick calculator (DIRECT TOOL)",layer==1&&focus==1);
#ifdef DESKTOP_PC_QWEN
 put(10,2,"PC QWEN: external model bridge. Tools execute on the desktop.");
#else
 put(10,2,"SCRIPTED: fixed prefix demo rules. No language model connected.");
#endif
 put(12,2,"Task / tool request / receipt / answer / clarification / error");
 put(14,2,"LAST EVENT:");text_box(15,4,74,2,last_event,0);
 put(18,2,"RESULT:");text_box(19,4,74,5,last_result,0);
 char id[8];task_text(id,task_id);put(25,2,"TASK ID:");put_utf(25,12,id);
 footer("K0/K1 CHOOSE  K2 OPEN  K4 BACK  HOLD K4 BOTTOM ACTIONS");
}
static void draw_tools(void){
 if(editor){draw_editor();return;}
 put(5,2,"DIRECT TOOLS / DETERMINISTIC EXECUTION");
 const char*items[]={"Calculator", "Calculate + save", "Set study timer (default 25 min)", "Pause / resume timer", "Extend timer (default +5 min)", "Cancel timer", "Add note"};
 for(unsigned i=0;i<7;i++)field(7+i,4,52,items[i],layer==1&&i==focus);
 put(16,2,"Timer status:");field(16,22,5,timer_name(app.runtime.state.timer),0);
 put(17,2,"Remaining seconds:");uint64_t now=now_ms();
 uint64_t rem=app.runtime.state.timer==AG_RUNNING?(app.runtime.state.deadline_ms>now?app.runtime.state.deadline_ms-now:0):app.runtime.state.remaining_ms;
 char n[20];num(n,(unsigned)(rem/1000));field(17,22,10,n,0);
 put(19,2,"RESULT:");text_box(20,4,74,5,last_result,0);
 footer("K0/K1 CHOOSE TOOL  K2 RUN / EDIT PARAMETERS  K3 BACK TO NAV");
}
static void draw_records(void){
 put(5,2,"VOLATILE NOTES + CALCULATION RESULTS");
 if(!app.runtime.state.note_count)put(8,3,"No records yet. Save a calculation or add a note.");
 else{
  unsigned n=app.runtime.state.note_count,first=subpage*4;if(first>=n)first=0;
  for(unsigned i=0;i<4&&first+i<n;i++){
   char ix[12];num(ix,first+i+1);put(7+i*4,2,"N");put(7+i*4,3,ix);
   text_box(7+i*4,8,70,3,app.runtime.state.notes[first+i],0);
  }
 }
 put(24,2,"Storage: VOLATILE RAM. Reset clears records.");
 footer("K2 NEXT RECORD PAGE  K3 BACK TO NAV  HOLD K4 BOTTOM ACTIONS");
}
static void draw_log(void){
 put(5,2,"BOUNDED EXECUTION LOG / NEWEST LAST");unsigned show=log_count<8?log_count:8;
 for(unsigned i=0;i<show;i++){
  unsigned ix=(log_start+log_count-show+i)%LOG_CAP;char row[100]="";
  cat(row,logs[ix].source,sizeof row);cat(row," ",sizeof row);cat(row,logs[ix].task,sizeof row);cat(row," ",sizeof row);
  cat(row,logs[ix].tool,sizeof row);cat(row," = ",sizeof row);cat(row,logs[ix].result,sizeof row);
  text_box(7+i*2,2,76,2,row,0);
 }
 put(24,2,"Overflow: newest 12 retained. Newest 8 displayed.");
 footer("K3 BACK TO NAV  HOLD K4 BOTTOM ACTIONS");
}
#ifdef DESKTOP_TRACE
static int input_self_test(void){static const char han[]="\xe8\xae\xa1\xe7\xae\x97";cp(pinyin,"ji",sizeof pinyin);pinyin_len=2;cand_focus=0;rebuild_candidates();if(cand_count<2)return 0;cand_focus=1;rebuild_candidates();if(cand_focus!=1||!candidate(cand_focus)[0])return 0;cp(pinyin,"jisuan",sizeof pinyin);pinyin_len=6;rebuild_candidates();if(cand_count!=1||cand_focus!=0)return 0;cp(input,han,sizeof input);input_len=6;utf8_backspace(input,&input_len);if(input_len!=3)return 0;utf8_backspace(input,&input_len);if(input_len!=0)return 0;input_len=INPUT_CAP;input[INPUT_CAP]=0;if(append("x"))return 0;input_len=pinyin_len=cand_focus=cand_count=0;input[0]=pinyin[0]=0;return 1;}
#endif
#ifdef DESKTOP_TRACE
static void trace_render(void){puts_uart("UI page=");putchar_uart('0'+(page%10));puts_uart(" layer=");putchar_uart('0'+(layer%10));puts_uart(" editor=");putchar_uart('0'+(editor%10));puts_uart(" zh=");putchar_uart('0'+(chinese%10));puts_uart(" key=");puthex(key_focus);puts_uart(" key_count=");puthex(KEY_COUNT);puts_uart(" cand_mode=");putchar_uart('0'+(candidate_mode%10));puts_uart(" cand=");puthex(cand_focus);putchar_uart('/');puthex(cand_count);puts_uart(" input_len=");puthex(input_len);puts_uart(" py=");puts_uart(pinyin);puts_uart(" text=");puts_uart(input);puts_uart(" event=");puts_uart(last_event);puts_uart(" result=");puts_uart(last_result);puts_uart(" status=");puts_uart(status_text);puts_uart(" notes=");puthex(app.runtime.state.note_count);puts_uart(" note0=");if(app.runtime.state.note_count)puts_uart(app.runtime.state.notes[0]);puts_uart(" timer=");puts_uart(timer_name(app.runtime.state.timer));puts_uart(" logs=");puthex(log_count);if(log_count){unsigned li=(log_start+log_count-1)%LOG_CAP;puts_uart(" last_source=");puts_uart(logs[li].source);puts_uart(" last_tool=");puts_uart(logs[li].tool);}puts_uart(" RENDER_READY\n");}
#endif
static void redraw(void){term_clear();title();if(page==0)draw_assistant();else if(page==1)draw_tools();else if(page==2)draw_records();else draw_log();term_commit();
#ifdef DESKTOP_TRACE
trace_render();
#endif
}

static ag_backend_poll_status scripted_backend(void*ctx,const ag_loop_input*in,uint32_t quota,char*out,size_t cap,size_t*used){(void)ctx;(void)quota;if(in->kind==AG_LOOP_INPUT_TASK){const char*mark="\xe8\xae\xa1\xe7\xae\x97";if(contains(in->task,mark)){const char*p=in->task;while(*p&&!( (unsigned char)p[0]==0xe8&&(unsigned char)p[1]==0xae&&(unsigned char)p[2]==0xa1))p++;if(*p)p+=6;while(*p==' '||*p==':')p++;if(!*p)return AG_BACKEND_FAILED;const char*pre="{\"type\":\"tool\",\"id\":\"script-calc\",\"request\":\"{\\\"name\\\":\\\"calculate\\\",\\\"arguments\\\":{\\\"expression\\\":\\\"";const char*post="\\\"}}\"}";size_t n=0;while(*pre){if(n>=cap)return AG_BACKEND_FAILED;out[n++]=*pre++;}while(*p){if(!((*p>='0'&&*p<='9')||*p=='+'||*p=='-'||*p=='*'||*p=='/'||*p=='.'||*p=='('||*p==')'))return AG_BACKEND_FAILED;if(n>=cap)return AG_BACKEND_FAILED;out[n++]=*p++;}while(*post){if(n>=cap)return AG_BACKEND_FAILED;out[n++]=*post++;}out[n]=0;*used=n;return AG_BACKEND_READY;}const char*clarify="{\"type\":\"clarify\",\"text\":\"SCRIPTED backend supports only 计算 followed by a numeric expression. No model connected.\"}";size_t n=slen(clarify);if(n>=cap)return AG_BACKEND_FAILED;for(size_t i=0;i<=n;i++)out[i]=clarify[i];*used=n;return AG_BACKEND_READY;}if(in->kind==AG_LOOP_INPUT_TOOL_RECEIPT){if(in->receipt_code!=AG_OK)return AG_BACKEND_FAILED;const char*f="{\"type\":\"finish\",\"text\":\"";size_t n=slen(f);if(n>=cap)return AG_BACKEND_FAILED;for(size_t i=0;i<n;i++)out[i]=f[i];const char*p=in->receipt_text;while(*p){if(n+4>=cap||*p=='\"'||*p=='\\')return AG_BACKEND_FAILED;out[n++]=*p++;}out[n++]='\"';out[n++]='}';out[n]=0;*used=n;return AG_BACKEND_READY;}return AG_BACKEND_FAILED;}
static void on_event(void*ctx,ag_loop_event_kind kind,const char*id,ag_code code,const char*text){(void)ctx;char tid[8],tool[24];task_text(tid,task_id);if(kind==AG_LOOP_EVENT_TOOL){cp(tool,id,sizeof tool);cp(last_event,"Tool requested",sizeof last_event);cp(last_result,text,sizeof last_result);log_event(BACKEND_NAME,tid,tool,code==AG_OK?text:ag_code_name(code));}else if(kind==AG_LOOP_EVENT_CLARIFICATION){error_visible=0;cp(last_event,"Clarification",sizeof last_event);cp(last_result,text,sizeof last_result);log_event(BACKEND_NAME,tid,"clarify",text);}else if(kind==AG_LOOP_EVENT_FINAL){error_visible=0;cp(last_event,"Completed",sizeof last_event);cp(last_result,text,sizeof last_result);log_event(BACKEND_NAME,tid,"finish",text);}else{cp(last_event,"Backend/error",sizeof last_event);cp(last_result,text,sizeof last_result);log_event(BACKEND_NAME,tid,"error",text);error_visible=1;}}
static int run_direct(const char*tool,const char*json){direct_tool(tool,json);redraw();return !error_visible;}
static void start_calculator(unsigned save){editor=1;chinese=0;input_len=pinyin_len=key_focus=0;candidate_mode=0;cand_focus=cand_count=0;input[0]=pinyin[0]=0;focus=save?1:0;redraw();}
static void exec_tool_item(unsigned item){if(item==0||item==1){start_calculator(item==1);return;}if(item==2||item==4){editor=1;chinese=0;input_len=pinyin_len=key_focus=0;candidate_mode=0;cand_focus=cand_count=0;cp(input,item==2?"25":"5",sizeof input);input_len=slen(input);redraw();return;}if(item==3){const char*j=app.runtime.state.timer==AG_RUNNING?"{\"name\":\"pause_timer\",\"arguments\":{}}":"{\"name\":\"resume_timer\",\"arguments\":{}}";run_direct(app.runtime.state.timer==AG_RUNNING?"pause_timer":"resume_timer",j);return;}if(item==5){run_direct("cancel_timer","{\"name\":\"cancel_timer\",\"arguments\":{}}");return;}if(item==6){editor=1;chinese=1;input_len=pinyin_len=key_focus=0;input[0]=pinyin[0]=0;redraw();return;}}
static void execute_editor(void){if(!input_len){cp(status_text,"EMPTY INPUT",sizeof status_text);error_visible=1;redraw();return;}if(page==0){if(app.status==AG_LOOP_RUNNING){cp(status_text,"TASK BUSY / K4 BACK CANCEL",sizeof status_text);redraw();return;}clear_receipts();task_id++;error_visible=0;last_result[0]=0;
#ifdef DESKTOP_PC_QWEN
desktop_pc_begin(task_id);
#endif
ag_loop_status st=ag_loop_submit(&app,input,now_ms(),TASK_TIMEOUT_MS,8);if(st!=AG_LOOP_RUNNING){cp(status_text,ag_loop_status_name(st),sizeof status_text);error_visible=1;log_event(BACKEND_NAME,"SUBMIT","ag_loop",status_text);}else{cp(status_text,BACKEND_NAME " RUNNING",sizeof status_text);cp(last_event,"Task submitted",sizeof last_event);log_event(BACKEND_NAME,"SUBMIT","task",input);puts_uart("DESK ASSIST SUBMIT ");puts_uart(input);puts_uart("\n");}editor=0;pinyin_len=0;redraw();return;}char json[256];if(page==1&&(focus==2||focus==4)){json[0]=0;if(focus==2){cat(json,"{\"name\":\"start_timer\",\"arguments\":{\"value\":",sizeof json);cat(json,input,sizeof json);cat(json,",\"unit\":\"分钟\",\"label\":\"Study\"}}",sizeof json);direct_tool("start_timer",json);}else{cat(json,"{\"name\":\"extend_timer\",\"arguments\":{\"value\":",sizeof json);cat(json,input,sizeof json);cat(json,",\"unit\":\"分钟\"}}",sizeof json);direct_tool("extend_timer",json);}editor=0;input_len=0;redraw();return;}if(page==1&&(focus==0||focus==1)){char tid[8];task_text(tid,++task_id);clear_receipts();char calcid[AG_ID_MAX+1];direct_id++;calcid[0]='D';num(calcid+1,direct_id);json[0]=0;cat(json,"{\"name\":\"calculate\",\"arguments\":{\"expression\":\"",sizeof json);cat(json,input,sizeof json);cat(json,"\"}}",sizeof json);ag_receipt r;ag_code c=ag_execute(&app.runtime,calcid,json,now_ms(),&r);cp(last_result,r.result,sizeof last_result);log_event("DIRECT",tid,"calculate",c?ag_code_name(c):r.result);if(!c&&focus==1){char saveid[AG_ID_MAX+1],savejson[180];direct_id++;saveid[0]='D';num(saveid+1,direct_id);savejson[0]=0;cat(savejson,"{\"name\":\"save_result\",\"arguments\":{\"result_id\":\"",sizeof savejson);cat(savejson,calcid,sizeof savejson);cat(savejson,"\"}}",sizeof savejson);c=ag_execute(&app.runtime,saveid,savejson,now_ms(),&r);log_event("DIRECT",tid,"save_result",c?ag_code_name(c):"saved");}clear_receipts();error_visible=c!=AG_OK;cp(status_text,c==AG_OK?"DIRECT OK":"DIRECT ERROR",sizeof status_text);cp(last_event,c?ag_code_name(c):"Calculator result",sizeof last_event);puts_uart(c?"DESK TOOL ERROR\n":"DESK TOOL RESULT ");if(!c){puts_uart(last_result);putchar_uart('\n');}editor=0;input_len=pinyin_len=0;redraw();return;}if(page==1&&focus==6){json[0]=0;cat(json,"{\"name\":\"append_note\",\"arguments\":{\"text\":\"",sizeof json);cat(json,input,sizeof json);cat(json,"\"}}",sizeof json);run_direct("append_note",json);editor=0;redraw();return;}cp(last_event,"Editor sent",sizeof last_event);editor=0;redraw();}
static void short_pick(void){if(editor){if(app.status!=AG_LOOP_RUNNING){error_visible=0;cp(status_text,"EDITING",sizeof status_text);}if(chinese&&candidate_mode&&cand_count){const char*w=candidate(cand_focus);if(!append(w)){cp(status_text,"INPUT FULL",sizeof status_text);error_visible=1;}else{pinyin_len=0;pinyin[0]=0;candidate_mode=0;rebuild_candidates();}}else if(chinese){if(pinyin_len+1>=sizeof pinyin){cp(status_text,"PINYIN FULL",sizeof status_text);error_visible=1;}else{pinyin[pinyin_len++]=KEYSET[key_focus];pinyin[pinyin_len]=0;candidate_mode=0;rebuild_candidates();}}else{char c=KEYSET[key_focus];if(input_len<INPUT_CAP){input[input_len++]=c;input[input_len]=0;}else{cp(status_text,"INPUT FULL",sizeof status_text);error_visible=1;}}redraw();return;}if(layer==0){layer=1;focus=0;redraw();return;}if(layer==1){if(page==0){if(focus==0){editor=1;chinese=1;input_len=pinyin_len=key_focus=0;input[0]=pinyin[0]=0;candidate_mode=0;rebuild_candidates();}else{page=1;focus=0;exec_tool_item(0);}}else if(page==1)exec_tool_item(focus);else if(page==2){unsigned n=app.runtime.state.note_count;subpage=n>4?(subpage+1)%((n+3)/4):0;}else subpage^=1;redraw();return;}if(layer==2){if(focus==0){page=0;editor=1;chinese=1;input_len=pinyin_len=key_focus=0;input[0]=pinyin[0]=0;candidate_mode=0;rebuild_candidates();}else if(focus==1){page=1;exec_tool_item(0);}else{layer=0;}redraw();}}
static void back(void){if(!editor&&app.status==AG_LOOP_RUNNING){ag_loop_cancel(&app);
#ifdef DESKTOP_PC_QWEN
desktop_pc_cancel();
#endif
cp(status_text,"CANCELLED",sizeof status_text);redraw();return;}if(editor){editor=0;input_len=pinyin_len=0;input[0]=pinyin[0]=0;layer=1;redraw();return;}if(layer==2){layer=1;redraw();return;}if(layer==1){layer=0;focus=page;redraw();return;}page=0;layer=0;redraw();}
static void send(void){if(editor){execute_editor();return;}if(layer==0){layer=2;focus=0;}else if(layer==1){layer=2;focus=0;}redraw();}
static void handle_buttons(void){uint32_t ev=button_events(),press=ev&BTN_PRESS_MASK,release=(ev>>5)&BTN_PRESS_MASK,longp=(ev>>10)&BTN_PRESS_MASK;
#ifdef DESKTOP_TRACE
if(ev & (BTN_PRESS_MASK|BTN_RELEASE_MASK|BTN_LONG_MASK)){puts_uart("BTN=");puthex(ev);putchar_uart('\n');}
#endif
unsigned k0=1u<<BTN_K0,k1=1u<<BTN_K1,k2=1u<<BTN_K2,k3=1u<<BTN_K3,k4=1u<<BTN_K4;if(press&k4)k4_armed=1;if(longp&k4){k4_armed=0;send();}if(release&k4){if(k4_armed){k4_armed=0;back();}}if(press&k0)k0_armed=1;if(press&k1)k1_armed=1;if((release&k0)&&k0_armed&&!(longp&k0)){k0_armed=0;if(editor&&candidate_mode&&cand_count)cand_focus=(cand_focus+cand_count-1)%cand_count;else if(editor)key_focus=(key_focus+KEY_COUNT-1)%KEY_COUNT;else if(layer==0)page=(page+3)%4;else if(layer==1)focus=focus?focus-1:((page==1)?6:page==0?1:0);else focus=focus?focus-1:2;redraw();}if((release&k1)&&k1_armed&&!(longp&k1)){k1_armed=0;if(editor&&candidate_mode&&cand_count)cand_focus=(cand_focus+1)%cand_count;else if(editor)key_focus=(key_focus+1)%KEY_COUNT;else if(layer==0)page=(page+1)%4;else if(layer==1)focus=(focus+1)%((page==1)?7:page==0?2:page==2?1:2);else focus=(focus+1)%3;redraw();}if(longp&k0){k0_armed=0;if(editor){if(candidate_mode&&cand_count){cand_focus=cand_focus>=5?((cand_focus/5)-1)*5:((cand_count-1)/5)*5;}else if(pinyin_len){pinyin[--pinyin_len]=0;candidate_mode=0;cand_focus=0;rebuild_candidates();}else utf8_backspace(input,&input_len);redraw();}}if(longp&k1){k1_armed=0;if(editor&&candidate_mode&&cand_count){cand_focus=(((cand_focus/5)+1)*5)%cand_count;redraw();}}if(press&k2)k2_armed=1;if(longp&k2){k2_armed=0;if(editor){if(chinese&&candidate_mode&&cand_count)short_pick();else{chinese^=1;pinyin_len=0;pinyin[0]=0;candidate_mode=0;rebuild_candidates();redraw();}}}if(release&k2){if(k2_armed){k2_armed=0;short_pick();}}if(press&k3){if(editor){if(cand_count)candidate_mode^=1;else if(pinyin_len){pinyin_len--;pinyin[pinyin_len]=0;rebuild_candidates();}else utf8_backspace(input,&input_len);redraw();}else back();}if(longp&k3&&editor){input_len=pinyin_len=0;input[0]=pinyin[0]=0;candidate_mode=0;rebuild_candidates();redraw();}
}
static void loop_tick(void){static unsigned shown_second;uint64_t c=read_cycles();(void)ag_tick(&app.runtime,now_ms());unsigned second=(unsigned)(now_ms()/1000u);if(second!=shown_second){shown_second=second;if(app.runtime.state.timer!=AG_IDLE)redraw();}if(app.status==AG_LOOP_RUNNING&&c-last_step_cycles>=DESKTOP_STEP_CYCLES
#ifdef DESKTOP_PC_QWEN
&&desktop_pc_poll_due()
#endif
){last_step_cycles=c;ag_loop_status s=ag_loop_step(&app,now_ms());if(s!=AG_LOOP_RUNNING&&s!=AG_LOOP_FINISHED&&s!=AG_LOOP_CLARIFY){error_visible=1;cp(status_text,ag_loop_status_name(s),sizeof status_text);}else cp(status_text,s==AG_LOOP_CLARIFY?BACKEND_NAME " CLARIFICATION":s==AG_LOOP_FINISHED?BACKEND_NAME " COMPLETE":BACKEND_NAME " RUNNING",sizeof status_text);redraw();}}
int main(void){puts_uart("DESK BOOT backend=" BACKEND_NAME " state=VOLATILE\n");
#ifdef DESKTOP_PC_QWEN
ag_loop_init(&app,(ag_loop_backend){desktop_pc_poll,0},on_event,0,0,0);
#else
ag_loop_init(&app,(ag_loop_backend){scripted_backend,0},on_event,0,0,0);
#endif
if(!han_resources_ready()){puts_uart("DESK HAN RESOURCES FAIL\n");return 1;}puts_uart("DESK HAN RESOURCES PASS\n");app_ready=1;(void)app_ready;
#ifdef DESKTOP_TRACE
if(!input_self_test()){puts_uart("DESK INPUT SELFTEST FAIL\n");return 1;}puts_uart("DESK INPUT SELFTEST PASS\n");
#endif
redraw();puts_uart("DESK PAGE ASSISTANT\nDESK DISPLAY READY\n");for(;;){handle_buttons();loop_tick();}}
