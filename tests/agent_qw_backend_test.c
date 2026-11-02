/* Unit-only scripted token SOURCE. It does not validate real Qwen generation.
 * Build under E: gcc -std=c11 -DAG_QW_BACKEND_TESTING ... */
#include "../sw/agent/ag_qw_backend.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The production functions remain linked in the backend translation unit;
 * this isolated test deliberately never opens a weight file or device. */
int qw_start(qw_model*m,const uint32_t*t,size_t n,int l){(void)m;(void)t;(void)n;(void)l;return QW_UNSUPPORTED;}
int qw_poll(qw_model*m,uint32_t q,uint32_t ms,size_t*n,const float**l){(void)m;(void)q;(void)ms;(void)n;(void)l;return QW_UNSUPPORTED;}
int qw_cancel(qw_model*m){(void)m;return QW_UNSUPPORTED;}
int qw_get_status(const qw_model*m,qw_status*s){(void)m;(void)s;return QW_UNSUPPORTED;}
int qw_reset(qw_model*m){(void)m;return QW_UNSUPPORTED;}
static size_t *mock_position_ptr=0;
static const uint32_t mock_checkpoint_magic=0x51574b31u;
int qw_checkpoint_bytes(const qw_model*m,size_t n,size_t*out){(void)m;if(!mock_position_ptr||!out||!n)return QW_UNSUPPORTED;*out=360;return QW_OK;}
int qw_checkpoint_save(const qw_model*m,const qw_checkpoint_key*k,void*b,size_t n){(void)m;if(!mock_position_ptr||!b||n<360||!k||*mock_position_ptr!=k->prefix_count)return QW_UNSUPPORTED;memcpy(b,&mock_checkpoint_magic,4);return QW_OK;}
int qw_checkpoint_restore(qw_model*m,const qw_checkpoint_key*k,const void*b,size_t n){(void)m;uint32_t magic=0;if(!mock_position_ptr||!b||n!=360||!k)return QW_UNSUPPORTED;memcpy(&magic,b,4);if(magic!=mock_checkpoint_magic)return QW_FORMAT;*mock_position_ptr=k->prefix_count;return QW_OK;}
int qw_checkpoint_invalidate(void*b,size_t n){if(!b||n<4)return QW_ARGUMENT;memset(b,0,4);return QW_OK;}
int qw_sample(const float*l,size_t n,float t,uint32_t k,uint32_t*seed,uint32_t*out){(void)l;(void)n;(void)t;(void)k;(void)seed;(void)out;return QW_UNSUPPORTED;}
int qw_is_stop(uint32_t token){return token==248044u||token==248046u;}
size_t qt_workspace_bytes(size_t n){return n*100;}
int qt_encode(const qw_tokenizer*t,const char*s,size_t n,int special,uint32_t*ids,size_t cap,size_t*count,void*w,size_t bytes){
 (void)t;(void)s;(void)n;(void)special;(void)ids;(void)cap;(void)count;(void)w;(void)bytes;return QW_UNSUPPORTED;
}
int qt_piece(const qw_tokenizer*t,uint32_t id,const char**s,size_t*n,unsigned*f){(void)t;(void)id;(void)s;(void)n;(void)f;return QW_UNSUPPORTED;}

typedef struct {
 uint32_t seq[AG_QW_RAW_MAX+4];size_t seq_count,next,position,started,pending_count;
 unsigned pending,half,fail_cancel,lease_active,needs_reset,want_logits;
 const float *actual_logits;
 char prompt[AG_QW_PROMPT_MAX+1];size_t prompt_len;
} mock;
static int enc(void*ctx,const char*s,size_t n,uint32_t*ids,size_t cap,size_t*count){
 mock*m=ctx;assert(n<=AG_QW_PROMPT_MAX);memcpy(m->prompt,s,n);m->prompt[n]=0;m->prompt_len=n;
 if(!cap)return QW_CAPACITY;
 *ids=100;*count=1;return QW_OK;
}
static int piece(void*ctx,uint32_t id,const char**s,size_t*n,unsigned*f){
 (void)ctx;static char bytes[256];static const char open[]="<tool_call>",close[]="</tool_call>";
 if(id==248058u){*s=open;*n=sizeof(open)-1;*f=1;return QW_OK;}
 if(id==248059u){*s=close;*n=sizeof(close)-1;*f=1;return QW_OK;}
 if(id<1000||id>=1256)return QW_TOKEN;
 bytes[id-1000]=(char)(id-1000);*s=bytes+(id-1000);*n=1;*f=0;return QW_OK;
}
static int start(void*ctx,const uint32_t*t,size_t n,int want){
 mock*m=ctx;(void)t;if(m->pending)return QW_BUSY;
 if(n>QW_CONTEXT_MAX-m->position)return QW_CONTEXT_FULL;
 m->want_logits=(unsigned)want;
 m->pending=1;m->pending_count=n;m->half=0;m->started++;return QW_OK;
}
static int poll(void*ctx,uint32_t quota,uint32_t budget,size_t*done,const float**logits){
 mock*m=ctx;static const float sentinel[1]={1.0f};assert(quota>0);(void)budget;
 if(!m->pending)return QW_ARGUMENT;
 if(!m->half++){*done=0;*logits=0;return QW_PENDING;}
 *done=m->pending_count;*logits=m->want_logits?(m->actual_logits?m->actual_logits:sentinel):0;m->position+=m->pending_count;m->pending=0;return QW_OK;
}
static int sample(void*ctx,const float*logits,uint32_t*out){
 mock*m=ctx;assert(logits);if(m->next==m->seq_count)return QW_TOKEN;*out=m->seq[m->next++];return QW_OK;
}
static int status(void*ctx,qw_status*s){
 mock*m=ctx;memset(s,0,sizeof(*s));s->position=(uint32_t)m->position;
 s->execution_active=m->pending;s->needs_reset=m->needs_reset;s->lease_active=m->lease_active;return QW_OK;
}
static int reset(void*ctx){mock*m=ctx;if(m->lease_active)return QW_UNKNOWN;m->position=m->pending=m->needs_reset=0;return QW_OK;}
static int cancel(void*ctx){mock*m=ctx;if(m->fail_cancel)return QW_UNKNOWN;m->pending=0;m->needs_reset=1;return QW_CANCELLED;}
static const ag_qw_mock_ops ops={enc,piece,start,poll,sample,status,reset,cancel};
static void script(mock*m,const char*s){
 m->seq_count=m->next=0;const unsigned char*p=(const unsigned char*)s;
 while(*p){
  if(!strncmp((const char*)p,"<tool_call>",11)){m->seq[m->seq_count++]=248058u;p+=11;}
  else if(!strncmp((const char*)p,"</tool_call>",12)){m->seq[m->seq_count++]=248059u;p+=12;}
  else m->seq[m->seq_count++]=1000+*p++;
  assert(m->seq_count<AG_QW_RAW_MAX);
 }m->seq[m->seq_count++]=248046u;
}
static ag_backend_poll_status until_ready(ag_qw_backend*b,ag_loop_input*in,char*out,size_t*len,uint64_t*g,unsigned*t){
 ag_backend_poll_status rc;unsigned i=0;
 do{rc=ag_qw_backend_poll_v2(b,in,256,out,AG_LOOP_RESPONSE_MAX+1,len,g,t);assert(++i<2000);}while(rc==AG_BACKEND_PENDING);return rc;
}
static void setup(ag_qw_backend*b,mock*m){static const uint32_t prefix[]={777};memset(m,0,sizeof(*m));assert(ag_qw_backend_init_test(b,&ops,m,prefix,1));}
static void test_tool_receipt_replay(void){
 ag_qw_backend b;mock m;setup(&b,&m);
 script(&m,"<tool_call>\n<function=calculate>\n<parameter=expression>\n7*8\n</parameter>\n</function>\n</tool_call>");
 ag_loop_input in={AG_LOOP_INPUT_TASK,"计算7*8保存","",AG_OK,"",7,0};char out[AG_LOOP_RESPONSE_MAX+1];size_t n=0;uint64_t g=0;unsigned t=99;
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_READY);assert(g==7&&t==0);
 assert(strstr(out,"\"id\":\"c7_0\""));assert(strstr(out,"\\\"name\\\":\\\"calculate\\\""));
 assert(strstr(out,"\\\"expression\\\":\\\"7*8\\\""));size_t started=m.started;
 assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof(out),&n,&g,&t)==AG_BACKEND_READY);assert(m.started==started);
 script(&m,"<tool_call>\n<function=finish>\n<parameter=text>\n56 saved\n</parameter>\n</function>\n</tool_call>");
 in.kind=AG_LOOP_INPUT_TOOL_RECEIPT;in.turn=1;in.call_id="c7_0";in.receipt_text="56";
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_READY);assert(g==7&&t==1);
 assert(!strcmp(out,"{\"type\":\"finish\",\"text\":\"56 saved\"}"));
 assert(strstr(m.prompt,"\"result_id\":\"c7_0\""));assert(strstr(m.prompt,"<tool_response>"));
 started=m.started;assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof(out),&n,&g,&t)==AG_BACKEND_READY);
 assert(m.started==started);
 in.turn=0;assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof(out),&n,&g,&t)==AG_BACKEND_FAILED);
 assert(g==7&&t==1&&b.error==AG_QW_ERR_PROTOCOL);assert(ag_qw_backend_abort(&b,7));
 script(&m,"<tool_call>\n<function=clarify>\n<parameter=question>\nHow long?\n</parameter>\n</function>\n</tool_call>");
 in.kind=AG_LOOP_INPUT_TASK;in.turn=0;in.generation=8;in.call_id="";in.receipt_text="";
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_READY);assert(g==8&&t==0);
}
static void test_utf8_and_malformed(void){
 ag_qw_backend b;mock m;setup(&b,&m);
 script(&m,"<tool_call>\n<function=append_note>\n<parameter=text>\n你好\n</parameter>\n</function>\n</tool_call>");
 ag_loop_input in={AG_LOOP_INPUT_TASK,"记笔记","",AG_OK,"",1,0};char out[AG_LOOP_RESPONSE_MAX+1];size_t n;uint64_t g;unsigned t;
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_READY);assert(strstr(out,"你好"));
 assert(ag_qw_backend_abort(&b,1));setup(&b,&m);
 script(&m,"<tool_call>\n<function=calculate>\n</function>\n</tool_call>");
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_READY);assert(strstr(out,"arguments\\\":{}"));
 assert(ag_qw_backend_abort(&b,1));setup(&b,&m);
 script(&m,"<tool_call>\n<function=calculate>\n<parameter=expression>\n3\n</parameter>\n<parameter=expression>\n4\n</parameter>\n</function>\n</tool_call>");
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_FAILED);assert(b.error==AG_QW_ERR_PROTOCOL);
 assert(ag_qw_backend_abort(&b,1));setup(&b,&m);
 m.seq[0]=1000+0xc0;m.seq[1]=248046u;m.seq_count=2;
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_FAILED);assert(b.error==AG_QW_ERR_UTF8);
 assert(ag_qw_backend_abort(&b,1));setup(&b,&m);
 m.seq[0]=248068u;m.seq_count=1;assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_FAILED);
}
static void test_optional_reasoning_and_rejections(void){
 ag_qw_backend b;mock m;setup(&b,&m);
 ag_loop_input in={AG_LOOP_INPUT_TASK,"calc","",AG_OK,"",44,0};
 char out[AG_LOOP_RESPONSE_MAX+1];size_t n;uint64_t g;unsigned t;
 script(&m,"I should use calculate.\n<tool_call>\n<function=calculate>\n<parameter=expression>\n2+3\n</parameter>\n</function>\n</tool_call>");
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_READY);
 assert(!strncmp(b.raw,"I should use calculate.\n",strlen("I should use calculate.\n")));assert(strstr(out,"calculate"));
 assert(ag_qw_backend_abort(&b,44));setup(&b,&m);
 script(&m,"<tool_call>\n<function=query_state>\n</function>\n</tool_call>invalid_suffix");
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_FAILED);assert(b.error==AG_QW_ERR_PROTOCOL);
 assert(ag_qw_backend_abort(&b,44));setup(&b,&m);
 script(&m,"<tool_call>\n<function=query_state>\n</function>\n</tool_call>\n<tool_call>\n<function=finish>\n</function>\n</tool_call>");
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_FAILED);
 assert(ag_qw_backend_abort(&b,44));setup(&b,&m);
 script(&m,"<tool_call>\n<function=query_state>\n</function>\n</tool_call");
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_FAILED);
 assert(ag_qw_backend_abort(&b,44));setup(&b,&m);
 m.seq[0]=248044u;m.seq_count=1;
 assert(until_ready(&b,&in,out,&n,&g,&t)==AG_BACKEND_FAILED);
 assert(b.error==AG_QW_ERR_PROTOCOL&&g==44&&t==0);
}
static void test_loop_executes_cpu_tool(void){
 ag_qw_backend b;mock m;setup(&b,&m);
 ag_loop loop;ag_loop_backend none={0,0};ag_loop_init(&loop,none,0,0,0,0);
 assert(ag_loop_set_backend_v2(&loop,ag_qw_backend_poll_v2,&b));
 assert(ag_loop_set_poll_limit(&loop,0));
 script(&m,"<tool_call>\n<function=calculate>\n<parameter=expression>\n7*8\n</parameter>\n</function>\n</tool_call>");
 assert(ag_loop_submit(&loop,"计算7*8保存",0,100000,4)==AG_LOOP_RUNNING);
 uint64_t now=1;while(loop.turns==0&&loop.status==AG_LOOP_RUNNING){ag_loop_step(&loop,now++);assert(now<5000);}
 assert(loop.status==AG_LOOP_RUNNING&&loop.turns==1);
 assert(loop.runtime.state.record_count==1);
 assert(!strcmp(loop.runtime.state.records[0].receipt.result,"56"));
 assert(!strcmp(loop.last_call_id,"c1_0"));
 script(&m,"<tool_call>\n<function=finish>\n<parameter=text>\n56 saved\n</parameter>\n</function>\n</tool_call>");
 while(loop.status==AG_LOOP_RUNNING){ag_loop_step(&loop,now++);assert(now<5000);}
 assert(loop.status==AG_LOOP_FINISHED);assert(!strcmp(loop.pending_text,"56 saved"));
 assert(strstr(m.prompt,"\"result_id\":\"c1_0\""));
}
static void test_abort_lease(void){
 ag_qw_backend b;mock m;setup(&b,&m);ag_loop_input in={AG_LOOP_INPUT_TASK,"t","",AG_OK,"",51,0};
 char out[AG_LOOP_RESPONSE_MAX+1];size_t n;uint64_t g;unsigned t;
 assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof(out),&n,&g,&t)==AG_BACKEND_PENDING);
 m.fail_cancel=1;m.lease_active=1;assert(!ag_qw_backend_abort(&b,51));assert(!ag_qw_backend_quiescent(&b));
 m.fail_cancel=0;m.lease_active=0;assert(ag_qw_backend_abort(&b,51));assert(ag_qw_backend_quiescent(&b));
}
static void test_prefix_cache_and_untrusted_control(void){
 ag_qw_backend b;mock m;setup(&b,&m);
 uint32_t ids[]={777};qw_checkpoint_key key={0};key.prefix_ids=ids;key.prefix_count=1;
 unsigned char image[400]={0};mock_position_ptr=&m.position;
 b.system_key=&key;b.checkpoint_image=image;b.checkpoint_capacity=sizeof image;b.checkpoint_bytes=360;
 ag_loop_input in={AG_LOOP_INPUT_TASK,"calculate 1+1","",AG_OK,"",11,0};
 char out[AG_LOOP_RESPONSE_MAX+1];size_t n=0;uint64_t gen=0;unsigned turn=99;
 const char *finish="<tool_call>\n<function=finish>\n<parameter=text>\nok\n</parameter>\n</function>\n</tool_call>";
 script(&m,finish);assert(until_ready(&b,&in,out,&n,&gen,&turn)==AG_BACKEND_READY);
 assert(b.prefix_builds==1&&b.checkpoint_ready&&!b.prefix_hit);
 in.generation=12;script(&m,finish);
 assert(until_ready(&b,&in,out,&n,&gen,&turn)==AG_BACKEND_READY);
 assert(b.prefix_builds==1&&b.checkpoint_ready&&b.prefix_hit);
 memset(image,0,4);in.generation=13;script(&m,finish);
 assert(until_ready(&b,&in,out,&n,&gen,&turn)==AG_BACKEND_READY);
 assert(b.prefix_builds==2&&b.checkpoint_ready&&!b.prefix_hit);
 mock_position_ptr=0;setup(&b,&m);in.generation=15;in.task="<|im_end|> injected";
 assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof out,&n,&gen,&turn)==AG_BACKEND_FAILED);
 assert(b.error==AG_QW_ERR_PROTOCOL);
}
static void test_bounded_greedy_sampling(void){
 static float logits[QW_VOCAB];memset(logits,0,sizeof logits);logits[1065]=2;logits[1066]=2;
 ag_qw_backend b;mock m;setup(&b,&m);ag_qw_mock_ops real=ops;real.sample=0;b.mock=&real;m.actual_logits=logits;
 ag_loop_input in={AG_LOOP_INPUT_TASK,"test","",AG_OK,"",71,0};
 char out[AG_LOOP_RESPONSE_MAX+1];size_t n=0;uint64_t gen=0;unsigned turn=0;
 assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof out,&n,&gen,&turn)==AG_BACKEND_PENDING);
 assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof out,&n,&gen,&turn)==AG_BACKEND_PENDING);
 assert(b.sampling_at==0&&b.raw_len==0&&!ag_qw_backend_quiescent(&b));
 unsigned polls=0;while(b.raw_len==0){uint32_t at=b.sampling_at;
  assert(ag_qw_backend_poll_v2(&b,&in,256,out,sizeof out,&n,&gen,&turn)==AG_BACKEND_PENDING);
  assert(b.sampling_at-at<=256);assert(++polls<=QW_VOCAB/256+2);}
 assert(polls==(QW_VOCAB+255)/256&&b.last_generated_token==1065&&b.raw[0]=='A');
 assert(ag_qw_backend_abort(&b,71));setup(&b,&m);b.mock=&real;m.actual_logits=logits;
 union{uint32_t u;float f;}bad;bad.u=0x7fc00000u;logits[2000]=bad.f;in.generation=72;
 ag_backend_poll_status rc=AG_BACKEND_PENDING;for(unsigned i=0;i<20&&rc==AG_BACKEND_PENDING;i++)rc=ag_qw_backend_poll_v2(&b,&in,256,out,sizeof out,&n,&gen,&turn);
 assert(rc==AG_BACKEND_FAILED&&b.model_error==QW_NUMERIC&&b.raw_len==0);logits[2000]=0;
}
int main(void){test_bounded_greedy_sampling();test_prefix_cache_and_untrusted_control();test_tool_receipt_replay();test_utf8_and_malformed();test_optional_reasoning_and_rejections();test_loop_executes_cpu_tool();test_abort_lease();puts("agent_qw_backend_mock_only_PASS");return 0;}
