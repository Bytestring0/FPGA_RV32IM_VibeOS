#include "ag_qw_backend.h"
#include "../model/qw_sha256.h"
#include <string.h>
#include <limits.h>

/* Prompt bytes, tokenizer and full IDs are pinned per configured version. */

enum { ST_IDLE=0, ST_WORK=1, ST_READY=2, ST_FAILED=3, ST_ABORTED=4, ST_SAMPLING=5,
 ST_CK_RESTORE=6,ST_CK_SAVE=7,ST_CK_RESET=8 };
enum { OP_SYSTEM_PREFIX=1, OP_PROMPT=2, OP_GENERATED=3 };
static size_t nlen(const char *s,size_t max) {size_t i=0;if(!s)return max+1;while(i<=max&&s[i])i++;return i;}
static int equal(const char *a,const char *b) {return a&&b&&!strcmp(a,b);}
static int untrusted_frame(const char *s){
 return !s||strstr(s,"<|")||strstr(s,"<tool_call")||strstr(s,"</tool_call")||
  strstr(s,"<tool_response")||strstr(s,"</tool_response");
}
static int blank(unsigned char c) {return c==' '||c=='\n'||c=='\r'||c=='\t';}
static int a_name(unsigned char c) {return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
static int a_key(unsigned char c) {return a_name(c)||(c>='0'&&c<='9');}
static int digest_present(const uint8_t h[32]){for(unsigned i=0;i<32;i++)if(h[i])return 1;return 0;}

#ifdef AG_QW_BACKEND_TESTING
static int tok_encode(ag_qw_backend*b,const char*s,size_t n,uint32_t*v,size_t cap,size_t*count){
 if(b->mock)return b->mock->encode(b->mock_context,s,n,v,cap,count);
 return qt_encode(b->tokenizer,s,n,1,v,cap,count,b->workspace,b->workspace_bytes);
}
static int tok_piece(ag_qw_backend*b,uint32_t id,const char**s,size_t*n,unsigned*f){
 return b->mock?b->mock->piece(b->mock_context,id,s,n,f):qt_piece(b->tokenizer,id,s,n,f);
}
static int model_start(ag_qw_backend*b,const uint32_t*t,size_t n,int log){
 return b->mock?b->mock->start(b->mock_context,t,n,log):qw_start(b->model,t,n,log);
}
static int model_poll(ag_qw_backend*b,uint32_t quota,size_t*done,const float**log){
 return b->mock?b->mock->poll(b->mock_context,quota,b->cpu_budget_ms,done,log):
  qw_poll(b->model,quota,b->cpu_budget_ms,done,log);
}
static int model_status(ag_qw_backend*b,qw_status*s){return b->mock?b->mock->status(b->mock_context,s):qw_get_status(b->model,s);}
static int model_reset(ag_qw_backend*b){return b->mock?b->mock->reset(b->mock_context):qw_reset(b->model);}
static int model_cancel(ag_qw_backend*b){return b->mock?b->mock->cancel(b->mock_context):qw_cancel(b->model);}
#else
static int tok_encode(ag_qw_backend*b,const char*s,size_t n,uint32_t*v,size_t cap,size_t*count){
 return qt_encode(b->tokenizer,s,n,1,v,cap,count,b->workspace,b->workspace_bytes);
}
static int tok_piece(ag_qw_backend*b,uint32_t id,const char**s,size_t*n,unsigned*f){return qt_piece(b->tokenizer,id,s,n,f);}
static int model_start(ag_qw_backend*b,const uint32_t*t,size_t n,int log){return qw_start(b->model,t,n,log);}
static int model_poll(ag_qw_backend*b,uint32_t quota,size_t*done,const float**log){return qw_poll(b->model,quota,b->cpu_budget_ms,done,log);}
static int model_status(ag_qw_backend*b,qw_status*s){return qw_get_status(b->model,s);}
static int model_reset(ag_qw_backend*b){return qw_reset(b->model);}
static int model_cancel(ag_qw_backend*b){return qw_cancel(b->model);}
#endif

static int put(char*dst,size_t cap,size_t*len,const char*src,size_t n){
 if(n>cap-*len)return 0;
 memcpy(dst+*len,src,n);*len+=n;dst[*len]=0;return 1;
}
static int lit(char*dst,size_t cap,size_t*len,const char*str){return put(dst,cap,len,str,strlen(str));}
static int quote(char*dst,size_t cap,size_t*len,const char*s,size_t n){
 static const char x[] = "0123456789abcdef";
 if(!lit(dst,cap,len,"\""))return 0;
 for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)s[i];
  if(c==0)return 0;
  if(c=='\"'||c=='\\'){char p[2]={'\\',(char)c};if(!put(dst,cap,len,p,2))return 0;}
  else if(c<32){char p[6]={'\\','u','0','0',x[c>>4],x[c&15]};if(!put(dst,cap,len,p,6))return 0;}
  else if(!put(dst,cap,len,(const char*)&s[i],1))return 0;
 }return lit(dst,cap,len,"\"");
}
static int decimal(char*dst,size_t cap,size_t*len,uint64_t x){
 char tmp[20];size_t n=0;do{tmp[n++]=(char)('0'+x%10);x/=10;}while(x);
 if(n>cap-*len)return 0;
 while(n){dst[(*len)++]=tmp[--n];}dst[*len]=0;return 1;
}
static int valid_number(const char*s,size_t n){
 size_t i=0;if(i<n&&s[i]=='-')i++;if(i==n)return 0;
 if(s[i]=='0')i++;else{if(s[i]<'1'||s[i]>'9')return 0;do{i++;}while(i<n&&s[i]>='0'&&s[i]<='9');}
 if(i<n&&s[i]=='.'){i++;size_t first=i;while(i<n&&s[i]>='0'&&s[i]<='9')i++;if(i==first)return 0;}
 return i==n&&n<=24; /* board's numeric JSON subset, never coerce */
}
static ag_backend_poll_status fail(ag_qw_backend*b,ag_qw_error e,int rc){
 b->error=e;b->model_error=rc;b->phase=ST_FAILED;return AG_BACKEND_FAILED;
}
static int utf8_put(ag_qw_backend*b,const char*s,size_t n){
 for(size_t i=0;i<n;i++){unsigned c=(unsigned char)s[i];
  if(!b->utf8_left){
   if(!c)return 0;
   if(c<128)continue;
   if(c>=0xc2&&c<=0xdf){b->utf8_left=1;b->utf8_min=0x80;b->utf8_max=0xbf;}
   else if(c>=0xe0&&c<=0xef){b->utf8_left=2;b->utf8_min=c==0xe0?0xa0:0x80;b->utf8_max=c==0xed?0x9f:0xbf;}
   else if(c>=0xf0&&c<=0xf4){b->utf8_left=3;b->utf8_min=c==0xf0?0x90:0x80;b->utf8_max=c==0xf4?0x8f:0xbf;}
   else return 0;
  }else{if(c<b->utf8_min||c>b->utf8_max)return 0;b->utf8_left--;b->utf8_min=0x80;b->utf8_max=0xbf;}
 }return put(b->raw,AG_QW_RAW_MAX,&b->raw_len,s,n);
}

/* Exact native Qwen block, no guessed missing fields or tool renames. */
static int native_to_json(ag_qw_backend*b){
 const char *s=b->raw,*p=strstr(s,"<tool_call>");
 /* The captured template permits optional reasoning BEFORE exactly one call.
  * Keep it verbatim in b->raw; reject embedded protocol tags and suffix text. */
 if(!p||(size_t)(p-s)>512)return 0;
 for(const char*q=s;q<p;q++)if(*q=='<'&&(
    !strncmp(q,"<tool_call",10)||!strncmp(q,"</tool_call",11)||
    !strncmp(q,"<function=",10)||!strncmp(q,"<parameter=",11)))return 0;
 p+=strlen("<tool_call>");if(*p++!='\n'||strncmp(p,"<function=",10))return 0;p+=10;
 const char *beg=p;while(a_key((unsigned char)*p))p++;
 size_t name_len=(size_t)(p-beg);if(!name_len||name_len>31||!a_name((unsigned char)*beg)||*p++!='>'||*p++!='\n')return 0;
 char name[32];memcpy(name,beg,name_len);name[name_len]=0;
 int terminal=equal(name,"finish")||equal(name,"clarify");
 char inner[AG_REQUEST_MAX+1]={0};size_t ilen=0;
 if(!terminal&&(!lit(inner,AG_REQUEST_MAX,&ilen,"{\"name\":")||
   !quote(inner,AG_REQUEST_MAX,&ilen,name,name_len)||!lit(inner,AG_REQUEST_MAX,&ilen,",\"arguments\":{")))return 0;
 char fields[3][32]={{0}};unsigned count=0;const char *terminal_value=0;size_t terminal_size=0;
 while(strncmp(p,"</function>",11)){
  if(strncmp(p,"<parameter=",11)||count==3)return 0;
  p+=11;beg=p;
  while(a_key((unsigned char)*p))p++;
  size_t key_n=(size_t)(p-beg);
  if(!key_n||key_n>31||!a_name((unsigned char)*beg)||*p++!='>'||*p++!='\n')return 0;
  for(unsigned i=0;i<count;i++)if(strlen(fields[i])==key_n&&!memcmp(fields[i],beg,key_n))return 0;
  memcpy(fields[count],beg,key_n);fields[count][key_n]=0;
  const char *val=p,*end=strstr(p,"\n</parameter>");if(!end)return 0;
  size_t n=(size_t)(end-val);p=end+strlen("\n</parameter>");if(*p++!='\n')return 0;
  if(terminal){if(count||!equal(fields[0],equal(name,"finish")?"text":"question"))return 0;
   terminal_value=val;terminal_size=n;}
  else{
   if(count&&!lit(inner,AG_REQUEST_MAX,&ilen,","))return 0;
   if(!quote(inner,AG_REQUEST_MAX,&ilen,fields[count],key_n)||!lit(inner,AG_REQUEST_MAX,&ilen,":"))return 0;
   if(equal(fields[count],"value")&&(equal(name,"start_timer")||equal(name,"set_timer")||equal(name,"extend_timer"))){
    if(!valid_number(val,n)||!put(inner,AG_REQUEST_MAX,&ilen,val,n))return 0;
   }else if(!quote(inner,AG_REQUEST_MAX,&ilen,val,n))return 0;
  }count++;
 }
 p+=11;if(*p++!='\n'||strncmp(p,"</tool_call>",12))return 0;p+=12;
 while(*p){if(!blank((unsigned char)*p++))return 0;}
 b->response_len=0;b->response[0]=0;
 if(terminal){if(count!=1||terminal_size>AG_LOOP_OUTPUT_MAX)return 0;
  return lit(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,equal(name,"finish")?"{\"type\":\"finish\",\"text\":":"{\"type\":\"clarify\",\"text\":")&&
   quote(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,terminal_value,terminal_size)&&
   lit(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,"}");
 }
 if(!lit(inner,AG_REQUEST_MAX,&ilen,"}}"))return 0;
 size_t id_n=0;b->pending_call_id[0]=0;
 if(!lit(b->pending_call_id,AG_ID_MAX,&id_n,"c")||
   !decimal(b->pending_call_id,AG_ID_MAX,&id_n,b->generation)||
   !lit(b->pending_call_id,AG_ID_MAX,&id_n,"_")||
   !decimal(b->pending_call_id,AG_ID_MAX,&id_n,b->turn))return 0;
 memcpy(b->last_tool_name,name,name_len+1);
 return lit(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,"{\"type\":\"tool\",\"id\":")&&
  quote(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,b->pending_call_id,id_n)&&
  lit(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,",\"request\":")&&
  quote(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,inner,ilen)&&
  lit(b->response,AG_LOOP_RESPONSE_MAX,&b->response_len,"}");
}

static int prompt(ag_qw_backend*b,const ag_loop_input*in){
 if(untrusted_frame(in->task)||
    (in->kind==AG_LOOP_INPUT_TOOL_RECEIPT&&untrusted_frame(in->receipt_text))){
  b->error=AG_QW_ERR_PROTOCOL;return 0;
 }
 char text[AG_QW_PROMPT_MAX+1]={0};size_t len=0,count=0,off=b->used;
 if(in->kind==AG_LOOP_INPUT_TASK){
  if(!lit(text,AG_QW_PROMPT_MAX,&len,"<|im_start|>user\n")||
    !lit(text,AG_QW_PROMPT_MAX,&len,in->task)||
    !lit(text,AG_QW_PROMPT_MAX,&len,"<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"))return 0;
  off=b->prefix_count;
 }else{
  size_t receipt_len=0;char receipt[AG_QW_PROMPT_MAX+1]={0};
  const char*code=ag_code_name(in->receipt_code);if(equal(code,"unknown"))return 0;
  if(!lit(receipt,AG_QW_PROMPT_MAX,&receipt_len,"{\"ok\":")||
     !lit(receipt,AG_QW_PROMPT_MAX,&receipt_len,in->receipt_code==AG_OK?"true":"false")||
     !lit(receipt,AG_QW_PROMPT_MAX,&receipt_len,",\"code\":")||
     !quote(receipt,AG_QW_PROMPT_MAX,&receipt_len,code,strlen(code))||
     !lit(receipt,AG_QW_PROMPT_MAX,&receipt_len,",\"text\":")||
     !quote(receipt,AG_QW_PROMPT_MAX,&receipt_len,in->receipt_text,strlen(in->receipt_text))||
     !lit(receipt,AG_QW_PROMPT_MAX,&receipt_len,",\"call_id\":")||
     !quote(receipt,AG_QW_PROMPT_MAX,&receipt_len,in->call_id,strlen(in->call_id)))return 0;
  if(equal(b->last_tool_name,"calculate")&&in->receipt_code==AG_OK){
   if(!lit(receipt,AG_QW_PROMPT_MAX,&receipt_len,",\"result_id\":")||
      !quote(receipt,AG_QW_PROMPT_MAX,&receipt_len,in->call_id,strlen(in->call_id)))return 0;
  }
  if(!lit(receipt,AG_QW_PROMPT_MAX,&receipt_len,"}")||
     !lit(text,AG_QW_PROMPT_MAX,&len,"\n<|im_start|>user\n<tool_response>\n")||
     !put(text,AG_QW_PROMPT_MAX,&len,receipt,receipt_len)||
     !lit(text,AG_QW_PROMPT_MAX,&len,"\n</tool_response><|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"))return 0;
 }
 if(off>=QW_CONTEXT_MAX)return 0;
 int rc=tok_encode(b,text,len,b->ids+off,QW_CONTEXT_MAX-off,&count);
 if(rc){b->model_error=rc;b->error=rc==QW_CAPACITY?AG_QW_ERR_CONTEXT:AG_QW_ERR_TOKENIZER;return 0;}
 if(!count||count>QW_CONTEXT_MAX-off){b->error=AG_QW_ERR_CONTEXT;return 0;}
 b->in_flight=count+(in->kind==AG_LOOP_INPUT_TASK&&b->used==0?b->prefix_count:0);
 b->operation=OP_PROMPT;b->stop_pending=0;
 rc=model_start(b,b->ids+b->used,b->in_flight,1);
 if(rc){b->model_error=rc;b->error=rc==QW_CONTEXT_FULL?AG_QW_ERR_CONTEXT:AG_QW_ERR_MODEL;return 0;}
 b->phase=ST_WORK;return 1;
}

int ag_qw_backend_init(ag_qw_backend*b,const ag_qw_config*c){
 if(!b)return 0;
 memset(b,0,sizeof(*b));
 if(!c||!c->model||!c->tokenizer||!c->tokenizer->data||!c->prefix_utf8||!c->prefix_ids||!c->system_key||
    !c->prefix_bytes||c->prefix_bytes>AG_QW_PREFIX_UTF8_MAX||!c->prefix_count||c->prefix_count>=QW_CONTEXT_MAX||
    !c->tokenizer_workspace||c->tokenizer_workspace_bytes<qt_workspace_bytes(c->prefix_bytes)){b->error=AG_QW_ERR_ARGUMENT;return 0;}
 qw_sha256 sha;unsigned char digest[32];
 
 qw_sha_init(&sha);qw_sha_update(&sha,c->prefix_utf8,c->prefix_bytes);qw_sha_final(&sha,digest);
 if(memcmp(digest,c->system_key->template_sha256,32)){b->error=AG_QW_ERR_PREFIX;return 0;}
 qw_sha_init(&sha);qw_sha_update(&sha,c->tokenizer->data,c->tokenizer->bytes);qw_sha_final(&sha,digest);
 if(memcmp(digest,c->system_key->tokenizer_sha256,32)){b->error=AG_QW_ERR_TOKENIZER;return 0;}
 b->model=c->model;b->tokenizer=c->tokenizer;b->workspace=c->tokenizer_workspace;
 b->workspace_bytes=c->tokenizer_workspace_bytes;b->cpu_budget_ms=c->cpu_budget_ms;
 qw_status s;if(qw_get_status(b->model,&s)||s.lease_active||s.needs_reset||s.execution_active||s.checkpoint_active||s.position){b->error=AG_QW_ERR_LEASE;return 0;}
 size_t n=0;int rc=tok_encode(b,c->prefix_utf8,c->prefix_bytes,b->ids,QW_CONTEXT_MAX,&n);
 if(rc||n!=c->prefix_count||memcmp(b->ids,c->prefix_ids,n*sizeof(uint32_t))){
  b->error=AG_QW_ERR_PREFIX;b->model_error=rc;return 0;
 }b->prefix_count=n;
 size_t checkpoint_bytes=0;
 if(!c->system_key||!c->checkpoint_image||c->checkpoint_candidate>1||
    !digest_present(c->system_key->tokenizer_sha256)||
    !digest_present(c->system_key->template_sha256)||
    !digest_present(c->system_key->schema_sha256)||
    c->system_key->prefix_count!=n||!c->system_key->prefix_ids||
    memcmp(c->system_key->prefix_ids,b->ids,n*sizeof(uint32_t))||
    qw_checkpoint_bytes(b->model,n,&checkpoint_bytes)||
    c->checkpoint_capacity<checkpoint_bytes){b->error=AG_QW_ERR_PREFIX;return 0;}
 b->system_key=c->system_key;b->checkpoint_image=c->checkpoint_image;
 b->checkpoint_capacity=c->checkpoint_capacity;b->checkpoint_bytes=checkpoint_bytes;
 b->checkpoint_ready=c->checkpoint_candidate;return 1;
}
#ifdef AG_QW_BACKEND_TESTING
int ag_qw_backend_init_test(ag_qw_backend*b,const ag_qw_mock_ops*ops,void*ctx,
 const uint32_t*ids,size_t n){
 if(!b||!ops||!ops->encode||!ops->piece||!ops->start||!ops->poll||
    !ops->status||!ops->reset||!ops->cancel||!ids||!n||n>=QW_CONTEXT_MAX)return 0;
 memset(b,0,sizeof(*b));b->mock=ops;b->mock_context=ctx;memcpy(b->ids,ids,n*sizeof(*ids));
 b->prefix_count=n;return 1;
}
#endif

static int same_input(const ag_qw_backend*b,const ag_loop_input*in){
 return in->generation==b->generation&&in->turn==b->turn&&in->kind==b->kind&&
  equal(in->task,b->task)&&
  (in->kind==AG_LOOP_INPUT_TASK ||
   (equal(in->call_id,b->receipt_call_id)&&in->receipt_code==(ag_code)b->receipt_code&&
    equal(in->receipt_text,b->receipt_text)));
}
static int begin(ag_qw_backend*b,const ag_loop_input*in){
 size_t task=nlen(in->task,AG_LOOP_TASK_MAX);if(!in->generation||task==0||task>AG_LOOP_TASK_MAX)return 0;
 if(in->kind==AG_LOOP_INPUT_TASK){
  if(in->turn!=0||(b->generation&&in->generation<=b->generation))return 0;
  if(b->generation&&!b->system_key){int rc=model_reset(b);if(rc){b->model_error=rc;b->error=AG_QW_ERR_LEASE;return 0;}}
  b->used=0;b->pending_call_id[0]=0;b->last_tool_name[0]=0;
 }else if(in->kind==AG_LOOP_INPUT_TOOL_RECEIPT){
  if(b->phase!=ST_READY||!b->last_was_tool||in->generation!=b->generation||
     b->turn==UINT_MAX||in->turn!=b->turn+1||!equal(in->task,b->task)||
     !equal(in->call_id,b->pending_call_id)||
     nlen(in->receipt_text,AG_RESULT_MAX)>AG_RESULT_MAX||
     (unsigned)in->receipt_code>(unsigned)AG_STORAGE)return 0;
  memcpy(b->receipt_text,in->receipt_text,strlen(in->receipt_text)+1);
  memcpy(b->receipt_call_id,in->call_id,strlen(in->call_id)+1);
 }else return 0;
 b->generation=in->generation;b->turn=in->turn;b->kind=(unsigned)in->kind;
 b->receipt_code=(unsigned)in->receipt_code;memcpy(b->task,in->task,task+1);
 b->raw[0]=0;b->raw_len=0;b->response[0]=0;b->response_len=0;b->utf8_left=0;
 b->generated_tokens=0;b->last_generated_token=UINT32_MAX;b->last_piece_flags=UINT_MAX;
 b->last_was_tool=0;b->has_result=0;b->error=AG_QW_ERR_NONE;b->model_error=0;
 if(in->kind==AG_LOOP_INPUT_TASK&&b->system_key){
  b->prefix_hit=0;
  if(b->checkpoint_ready){
   int restored=qw_checkpoint_restore(b->model,b->system_key,
     b->checkpoint_image,b->checkpoint_bytes);
   if(restored==QW_OK){b->used=b->prefix_count;b->prefix_hit=1;}
   else if(restored==QW_FORMAT||restored==QW_HASH||restored==QW_TOKEN){
    if(qw_checkpoint_invalidate(b->checkpoint_image,b->checkpoint_capacity)){
     b->error=AG_QW_ERR_PREFIX;b->phase=ST_FAILED;return 0;
    }b->checkpoint_ready=0;
   }else{b->model_error=restored;b->error=AG_QW_ERR_LEASE;b->phase=ST_FAILED;return 0;}
  }
  if(!b->checkpoint_ready){
   int started=model_start(b,b->ids,b->prefix_count,0);
   if(started){b->model_error=started;b->error=AG_QW_ERR_MODEL;b->phase=ST_FAILED;return 0;}
   b->in_flight=b->prefix_count;b->operation=OP_SYSTEM_PREFIX;
   b->prefix_builds++;b->phase=ST_WORK;return 1;
  }
 }
 if(!prompt(b,in)){if(b->error==AG_QW_ERR_NONE)b->error=AG_QW_ERR_PROTOCOL;b->phase=ST_FAILED;return 0;}
 return 1;
}

static ag_backend_poll_status schedule_token(ag_qw_backend*b,uint32_t id){
 int rc=0;if(id>=QW_VOCAB)return fail(b,AG_QW_ERR_MODEL,QW_TOKEN);
 b->last_generated_token=id;b->generated_tokens++;b->last_piece_flags=UINT_MAX;
 b->stop_pending=qw_is_stop(id)?1u:0u;
 if(b->stop_pending){if(id!=248046u||b->utf8_left)return fail(b,AG_QW_ERR_PROTOCOL,QW_TOKEN);}
 else{const char *part=0;size_t n=0;unsigned flags=0;rc=tok_piece(b,id,&part,&n,&flags);
  b->last_piece_flags=flags;
   if(rc||!part||flags>2)return fail(b,AG_QW_ERR_TOKENIZER,rc?rc:QW_TOKEN);
  if(flags==1&&id!=248058u&&id!=248059u)return fail(b,AG_QW_ERR_PROTOCOL,QW_TOKEN);
  if(!utf8_put(b,part,n))return fail(b,AG_QW_ERR_UTF8,QW_TOKEN);
 }
 if(b->used>=QW_CONTEXT_MAX)return fail(b,AG_QW_ERR_CONTEXT,QW_CONTEXT_FULL);
 b->ids[b->used]=id;b->in_flight=1;b->operation=OP_GENERATED;
 rc=model_start(b,b->ids+b->used,1,b->stop_pending?0:1);
 if(rc)return fail(b,rc==QW_CONTEXT_FULL?AG_QW_ERR_CONTEXT:AG_QW_ERR_MODEL,rc);
 b->phase=ST_WORK;b->sampling_logits=0;return AG_BACKEND_PENDING;
}

static ag_backend_poll_status begin_sampling(ag_qw_backend*b,const float*logits){
#ifdef AG_QW_BACKEND_TESTING
 if(b->mock&&b->mock->sample){uint32_t id=0;int rc=b->mock->sample(b->mock_context,logits,&id);
  if(rc)return fail(b,AG_QW_ERR_MODEL,rc);return schedule_token(b,id);}
#endif
 if(!logits)return fail(b,AG_QW_ERR_MODEL,QW_FORMAT);
 b->sampling_logits=logits;b->sampling_at=0;b->sampling_best=0;
 b->sampling_max=0;b->phase=ST_SAMPLING;return AG_BACKEND_PENDING;
}
static ag_backend_poll_status sampling_poll(ag_qw_backend*b,uint32_t quota){
 uint32_t take=quota;if(take>256)take=256;
 uint32_t end=b->sampling_at+take;if(end>QW_VOCAB)end=QW_VOCAB;
 for(uint32_t i=b->sampling_at;i<end;i++){
  union{float f;uint32_t u;}v;v.f=b->sampling_logits[i];
  if((v.u&0x7f800000u)==0x7f800000u)return fail(b,AG_QW_ERR_MODEL,QW_NUMERIC);
  if(i==0||v.f>b->sampling_max){b->sampling_max=v.f;b->sampling_best=i;}
 }
 b->sampling_at=end;
 return end==QW_VOCAB?schedule_token(b,b->sampling_best):AG_BACKEND_PENDING;
}

ag_backend_poll_status ag_qw_backend_poll_v2(void*ctx,const ag_loop_input*in,uint32_t quota,
 char*out,size_t capacity,size_t*out_n,uint64_t*result_gen,unsigned*result_turn){
 ag_qw_backend*b=ctx;if(out_n)*out_n=0;if(result_gen)*result_gen=0;if(result_turn)*result_turn=UINT_MAX;
 if(out&&capacity)out[0]=0;
 if(!b||!in||!quota||!out||!out_n||!result_gen||!result_turn||!capacity)return AG_BACKEND_FAILED;
 if(b->phase==ST_FAILED){*result_gen=b->generation;*result_turn=b->turn;return AG_BACKEND_FAILED;}
 if(b->phase==ST_ABORTED&&in->generation<=b->generation){*result_gen=b->generation;*result_turn=b->turn;return AG_BACKEND_FAILED;}
 if(b->phase==ST_SAMPLING){
  *result_gen=b->generation;*result_turn=b->turn;
  if(!same_input(b,in))return fail(b,AG_QW_ERR_PROTOCOL,QW_ARGUMENT);
  return sampling_poll(b,quota);
 }
 if(b->phase==ST_READY&&same_input(b,in)){
  *result_gen=b->generation;*result_turn=b->turn;
  if(b->response_len>=capacity)return AG_BACKEND_FAILED;
  memcpy(out,b->response,b->response_len+1);*out_n=b->response_len;return AG_BACKEND_READY;
 }
 if(b->phase==ST_WORK||(b->phase==ST_READY&&in->generation==b->generation&&
    (in->turn<=b->turn||!b->last_was_tool))){
  *result_gen=b->generation;*result_turn=b->turn;
  if(b->phase==ST_WORK&&!same_input(b,in))return fail(b,AG_QW_ERR_PROTOCOL,QW_ARGUMENT);
  if(b->phase==ST_READY&&in->turn<=b->turn)return fail(b,AG_QW_ERR_PROTOCOL,QW_ARGUMENT);
 }
 if(b->phase!=ST_WORK&&!begin(b,in)){*result_gen=b->generation;*result_turn=b->turn;return AG_BACKEND_FAILED;}
 size_t done=0;const float*logits=0;int rc=model_poll(b,quota,&done,&logits);
 if(rc==QW_PENDING)return AG_BACKEND_PENDING;
 *result_gen=b->generation;*result_turn=b->turn;
 if(rc||done!=b->in_flight)return fail(b,AG_QW_ERR_MODEL,rc?rc:QW_FORMAT);
 b->used+=done;
 if(b->operation==OP_SYSTEM_PREFIX){
  if(logits||b->used!=b->prefix_count)return fail(b,AG_QW_ERR_MODEL,QW_FORMAT);
  int saved=qw_checkpoint_save(b->model,b->system_key,
    b->checkpoint_image,b->checkpoint_capacity);
  if(saved)return fail(b,AG_QW_ERR_PREFIX,saved);
  b->checkpoint_ready=1;
  if(!prompt(b,in))return fail(b,b->error?b->error:AG_QW_ERR_PROTOCOL,b->model_error);
  return AG_BACKEND_PENDING;
 }
 if(b->operation==OP_GENERATED&&b->stop_pending){
  if(!b->raw_len||b->utf8_left||!native_to_json(b))return fail(b,AG_QW_ERR_PROTOCOL,QW_FORMAT);
  b->last_was_tool=strstr(b->response,"\"type\":\"tool\"")!=0;
  b->phase=ST_READY;b->has_result=1;
  if(b->response_len>=capacity)return fail(b,AG_QW_ERR_OUTPUT,QW_CAPACITY);
  memcpy(out,b->response,b->response_len+1);*out_n=b->response_len;return AG_BACKEND_READY;
 }
 if(!logits)return fail(b,AG_QW_ERR_MODEL,QW_FORMAT);
 return begin_sampling(b,logits);
}
int ag_qw_backend_quiescent(const ag_qw_backend*source){
 if(!source)return 0;
 ag_qw_backend*b=(ag_qw_backend*)source;qw_status s;
 return b->phase!=ST_WORK&&b->phase!=ST_SAMPLING&&!model_status(b,&s)&&!s.execution_active&&!s.lease_active;
}
int ag_qw_backend_abort(ag_qw_backend*b,uint64_t gen){
 if(!b||!gen||b->generation!=gen)return 0;
 qw_status status;if(model_status(b,&status)){b->error=AG_QW_ERR_LEASE;return 0;}
 if(status.execution_active){int rc=model_cancel(b);if(rc!=QW_OK&&rc!=QW_CANCELLED){b->model_error=rc;b->error=AG_QW_ERR_LEASE;return 0;}}
 b->phase=ST_FAILED;
 if(!ag_qw_backend_quiescent(b)){b->error=AG_QW_ERR_LEASE;return 0;}
 int rc=model_reset(b);if(rc){b->model_error=rc;b->error=AG_QW_ERR_LEASE;return 0;}
 if(!ag_qw_backend_quiescent(b)){b->error=AG_QW_ERR_LEASE;return 0;}
 b->phase=ST_ABORTED;b->error=AG_QW_ERR_CANCELLED;return 1;
}
