/* Real-model, host-only evaluator capture. Never links scripted backend mocks.
 * Loads configured W4/BF16 software model once; runs only explicit TSV tasks. */
#define _POSIX_C_SOURCE 200809L
#include "ag_qw_backend.h"
#include "qw_sha256.h"
#include "qw_json.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

typedef struct {FILE *f;uint64_t bytes,bytes_read;} file_source;
typedef struct {uint64_t now;unsigned kind,turn;char response[AG_LOOP_RESPONSE_MAX+1],raw[AG_QW_RAW_MAX+1];
 char id[AG_ID_MAX+1],request[AG_REQUEST_MAX+1],text[AG_LOOP_OUTPUT_MAX+1];ag_code code;
 unsigned has_receipt,input_kind;char receipt_id[AG_ID_MAX+1],receipt_text[AG_RESULT_MAX+1];ag_code receipt_code;
 int backend_error,model_error;uint32_t last_token,generated_tokens;
} captured_turn;
static struct {ag_loop *loop;ag_qw_backend *backend;captured_turn turns[AG_LOOP_MAX_TURNS+1];unsigned count;uint64_t now,setup_now;unsigned has_setup;ag_receipt setup_receipt;} cap;
static int read_at(void *ctx,uint64_t at,void *dst,size_t n){file_source*s=ctx;if(at>s->bytes||n>s->bytes-at||fseeko(s->f,(off_t)at,SEEK_SET))return -1;if(fread(dst,1,n,s->f)!=n)return -1;s->bytes_read+=n;return 0;}
static npu_storage_source open_source(const char*p,file_source*s){s->f=fopen(p,"rb");if(!s->f||fseeko(s->f,0,SEEK_END))abort();off_t n=ftello(s->f);if(n<0)abort();s->bytes=(uint64_t)n;s->bytes_read=0;return(npu_storage_source){read_at,s,s->bytes};}
static void *read_file(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f||fseeko(f,0,SEEK_END))abort();off_t z=ftello(f);if(z<0||fseeko(f,0,SEEK_SET))abort();void*b=malloc((size_t)z+1);if(!b||fread(b,1,(size_t)z,f)!=(size_t)z||fclose(f))abort();((char*)b)[z]=0;*n=(size_t)z;return b;}
static uint64_t clock_ms(void*ctx){(void)ctx;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return(uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void hash(const void*p,size_t n,uint8_t out[32]){qw_sha256 h;qw_sha_init(&h);qw_sha_update(&h,p,n);qw_sha_final(&h,out);}
static int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static void parse_hash(const char*s,uint8_t out[32]){if(!s||strlen(s)!=64)abort();for(unsigned i=0;i<32;i++){int a=nibble(s[i*2]),b=nibble(s[i*2+1]);if(a<0||b<0)abort();out[i]=(uint8_t)(a*16+b);}}
static void qstr(const char*s){putchar('"');for(const unsigned char*p=(const unsigned char*)s;*p;p++){if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}else if(*p<32)printf("\\u%04x",*p);else putchar(*p);}putchar('"');}
static void state_json(const ag_state*s){static const char*ts[]={"idle","running","paused","finished"};
 printf("{\"notes\":[");for(unsigned i=0;i<s->note_count;i++){if(i)putchar(',');qstr(s->notes[i]);}
 printf("],\"note_count\":%u,\"records\":[",s->note_count);
 for(unsigned i=0;i<s->record_count;i++){const ag_record*r=&s->records[i];if(i)putchar(',');printf("{\"id\":");qstr(r->id);printf(",\"tool\":");qstr(r->tool);printf(",\"request\":");qstr(r->request);printf(",\"receipt\":{\"code\":%u,\"result\":",(unsigned)r->receipt.code);qstr(r->receipt.result);printf("}}");}
 printf("],\"record_count\":%u,\"timer\":",s->record_count);qstr((unsigned)s->timer<4?ts[s->timer]:"unknown");printf(",\"label\":");qstr(s->label);
 printf(",\"deadline_ms\":%" PRIu64 ",\"remaining_ms\":%" PRIu64 ",\"timer_revision\":%u}",s->deadline_ms,s->remaining_ms,s->timer_revision);
}
static int response_parts(const char*response,char*id,size_t icap,char*request,size_t rcap){qj_doc d={response,strlen(response)};qj_value root,v;if(qj_read(d,&(size_t){0},&root))return 0;size_t at=0;if(qj_read(d,&at,&root)||at!=d.n||qj_get(d,root,"id",&v)||qj_text(d,v,id,icap))return 0;if(qj_get(d,root,"request",&v)||qj_text(d,v,request,rcap))return 0;return 1;}
static void on_event(void*ctx,uint64_t gen,unsigned turn,ag_loop_event_kind kind,const char*id,ag_code code,const char*text){(void)ctx;if(cap.count>=AG_LOOP_MAX_TURNS+1)abort();captured_turn*t=&cap.turns[cap.count++];memset(t,0,sizeof(*t));t->now=cap.now;t->kind=(unsigned)kind;t->turn=turn;t->code=code;
 ag_qw_backend*b=cap.backend;if(kind!=AG_LOOP_EVENT_ERROR&&(gen!=b->generation||turn!=b->turn))abort();snprintf(t->response,sizeof(t->response),"%s",b->response);snprintf(t->raw,sizeof(t->raw),"%s",b->raw);snprintf(t->text,sizeof(t->text),"%s",text?text:"");t->input_kind=b->kind;t->backend_error=(int)b->error;t->model_error=b->model_error;t->last_token=b->last_generated_token;t->generated_tokens=b->generated_tokens;
   
  if(b->kind==AG_LOOP_INPUT_TOOL_RECEIPT){t->has_receipt=1;snprintf(t->receipt_id,sizeof(t->receipt_id),"%s",b->receipt_call_id);snprintf(t->receipt_text,sizeof(t->receipt_text),"%s",b->receipt_text);t->receipt_code=(ag_code)b->receipt_code;}
  if(kind==AG_LOOP_EVENT_TOOL){if(strcmp(id,b->pending_call_id)||!response_parts(b->response,t->id,sizeof(t->id),t->request,sizeof(t->request)))abort();
  }
}
static void emit_case(const char*taskid,const char*casehash,uint64_t gen,const ag_state*initial,const ag_state*final,ag_loop_status status,uint64_t elapsed,uint64_t start){
 printf("{\"task_id\":");qstr(taskid);printf(",\"case_sha256\":");qstr(casehash);printf(",\"generation\":%" PRIu64 ",\"initial_state\":",gen);state_json(initial);printf(",\"final_state\":");state_json(final);printf(",\"final_status\":");qstr(ag_loop_status_name(status));printf(",\"started_monotonic_ms\":%" PRIu64 ",\"elapsed_ms\":%" PRIu64 ",\"prefix_hit\":%u,\"setup_events\":[",start,elapsed,cap.backend->prefix_hit);
 if(cap.has_setup){printf("{\"kind\":\"cpu_setup\",\"origin\":\"ag_execute\",\"id\":\"setup_timer\",\"request\":");qstr("{\"name\":\"start_timer\",\"arguments\":{\"value\":120,\"unit\":\"分钟\",\"label\":\"Existing\"}}");printf(",\"code\":%u,\"text\":",(unsigned)cap.setup_receipt.code);qstr(cap.setup_receipt.result);printf(",\"now_ms\":%" PRIu64 "}",cap.setup_now);}printf("],\"turns\":[");
 for(unsigned i=0;i<cap.count;i++){captured_turn*t=&cap.turns[i];if(i)putchar(',');printf("{\"generation\":%" PRIu64 ",\"turn\":%u,\"model_response\":",gen,t->turn);qstr(t->response);printf(",\"raw_model_generation\":");qstr(t->raw);
  if(t->kind==AG_LOOP_EVENT_TOOL){printf(",\"tool_event\":{\"kind\":\"tool\",\"origin\":\"ag_execute\",\"id\":");qstr(t->id);printf(",\"generation\":%" PRIu64 ",\"request\":",gen);qstr(t->request);printf(",\"code\":%u,\"text\":",(unsigned)t->code);qstr(t->text);printf(",\"now_ms\":%" PRIu64 "}",t->now);
   if(t->has_receipt){printf(",\"receipt_input\":{\"kind\":\"receipt\",\"call_id\":");qstr(t->receipt_id);printf(",\"generation\":%" PRIu64 ",\"code\":%u,\"text\":",gen,(unsigned)t->receipt_code);qstr(t->receipt_text);putchar('}');}}
      if(t->kind!=AG_LOOP_EVENT_TOOL&&t->has_receipt){printf(",\"receipt_input\":{\"kind\":\"receipt\",\"call_id\":");qstr(t->receipt_id);printf(",\"generation\":%" PRIu64 ",\"code\":%u,\"text\":",gen,(unsigned)t->receipt_code);qstr(t->receipt_text);putchar('}');}
   if(t->kind==AG_LOOP_EVENT_ERROR){printf(",\"loop_error\":{\"event_text\":");qstr(t->text);printf(",\"backend_error\":%d,\"model_error\":%d,\"last_generated_token\":%u,\"generated_tokens\":%u}",t->backend_error,t->model_error,t->last_token,t->generated_tokens);}
   else if(t->kind==AG_LOOP_EVENT_FINAL||t->kind==AG_LOOP_EVENT_CLARIFICATION){printf(",\"terminal_event\":{\"kind\":");qstr(t->kind==AG_LOOP_EVENT_FINAL?"final":"clarification");printf(",\"generation\":%" PRIu64 ",\"text\":",gen);qstr(t->text);putchar('}');}
  printf(",\"timestamp_monotonic_ms\":%" PRIu64 "}",t->now);
 }
 puts("]}");fflush(stdout);
}
static int run_one(ag_loop*loop,ag_qw_backend*b,const char*id,const char*casehash,const char*family,const char*prompt){
 ag_init(&loop->runtime,0,0); /* independent task state; keep generation/backend monotone */
 cap.has_setup=0;cap.setup_now=0;memset(&cap.setup_receipt,0,sizeof(cap.setup_receipt));uint64_t setup_now=clock_ms(0);ag_receipt setup_receipt;int has_setup=0;
 if(!strcmp(family,"timer_set")){const char*req="{\"name\":\"start_timer\",\"arguments\":{\"value\":120,\"unit\":\"分钟\",\"label\":\"Existing\"}}";ag_code rc=ag_execute(&loop->runtime,"setup_timer",req,setup_now,&setup_receipt);if(rc!=AG_OK)return 2;has_setup=1;cap.has_setup=1;cap.setup_now=setup_now;cap.setup_receipt=setup_receipt;}
 ag_state initial=loop->runtime.state;cap.count=0;uint64_t start=clock_ms(0);if(ag_loop_submit(loop,prompt,start,7200000u,AG_LOOP_MAX_TURNS)!=AG_LOOP_RUNNING)return 3;
 while(loop->status==AG_LOOP_RUNNING){uint64_t now=clock_ms(0);(void)ag_tick(&loop->runtime,now);cap.now=now;ag_loop_step(loop,now);}
 if(loop->awaiting_quiescence){if(ag_qw_backend_abort(b,loop->generation)&&ag_qw_backend_quiescent(b))(void)ag_loop_ack_quiescent(loop,loop->generation);}
 if(!b->prefix_hit)fprintf(stderr,"PREFIX_MISS_CASE id=%s\n",id);
 uint64_t end=clock_ms(0);fprintf(stderr,"TASK id=%s generation=%" PRIu64 " status=%s turns=%u prefix_hit=%u setup_timer=%u\n",id,loop->generation,ag_loop_status_name(loop->status),cap.count,b->prefix_hit,has_setup);
 (void)setup_receipt;
 if(has_setup)fprintf(stderr,"SETUP id=%s kind=cpu_ag_execute_start_timer code=ok call_id=setup_timer not_counted_as_model_turn=1\n",id);
 emit_case(id,casehash,loop->generation,&initial,&loop->runtime.state,loop->status,end-start,start);
  if(loop->awaiting_quiescence || !ag_qw_backend_quiescent(b)){fputs("UNQUIESCENT_ABORT",stderr);return 5;}
  return 0;
}
int main(int argc,char**argv){
 if(argc!=16){fprintf(stderr,"usage: agent_qw_eval_host W4 CPU_RAW CATALOG.JSON TOKEN.BIN PREFIX.txt PREFIX_IDS.bin tools.openai.json selected_tasks.tsv CHECKPOINT W4_SHA CPU_SHA CATALOG_SHA TOKEN_SHA SCHEMA_SHA TASKS_SHA\n");return 2;}
 file_source fs[3];npu_storage_source sources[3];uint8_t ids[3][32];for(unsigned i=0;i<3;i++)sources[i]=open_source(argv[i+1],&fs[i]);parse_hash(argv[10],ids[0]);parse_hash(argv[11],ids[1]);parse_hash(argv[12],ids[2]);
 size_t token_n,prefix_n,pid_n,schema_n,tasks_n,checkpoint_n;void*token=read_file(argv[4],&token_n),*prefix=read_file(argv[5],&prefix_n),*pid_data=read_file(argv[6],&pid_n),*schema=read_file(argv[7],&schema_n),*task_data=read_file(argv[8],&tasks_n),*checkpoint=read_file(argv[9],&checkpoint_n);
 uint8_t schema_sha[32],task_sha[32],actual[32];parse_hash(argv[14],schema_sha);parse_hash(argv[15],task_sha);hash(schema,schema_n,actual);if(memcmp(schema_sha,actual,32))abort();hash(task_data,tasks_n,actual);if(memcmp(task_sha,actual,32))abort();if(pid_n<8||prefix_n>AG_QW_PREFIX_UTF8_MAX)abort();uint32_t prefix_count;memcpy(&prefix_count,pid_data,4);if(!prefix_count||prefix_count>=QW_CONTEXT_MAX||pid_n!=4+(size_t)prefix_count*4)abort();
 uint8_t token_sha[32];parse_hash(argv[13],token_sha);qw_tokenizer tokenizer;if(qt_open(&tokenizer,token,token_n,token_sha)!=QW_OK)abort();
 qw_options o={0};o.context_tokens=2048;o.prefill_chunk_tokens=128;o.tile_rows=512;o.arena_ddr_offset=1u<<20;o.system_reserved_bytes=1u<<20;o.mode=NPU_RAW_SOFTWARE_REFERENCE;o.budgets=(npu_raw_budgets){10000000,1000000};o.weight_sha256=ids[0];o.cpu_sha256=ids[1];o.catalog_sha256=ids[2];o.now_ms=clock_ms;
 qw_memory_plan plan;size_t capacity=NPU_RAW_DDR_LIMIT-o.arena_ddr_offset;int rc=qw_plan(&o,sources[0].size_bytes,sources[1].size_bytes,sources[2].size_bytes,capacity,&plan);if(rc){fprintf(stderr,"PLAN_ERROR=%d\n",rc);return 1;}void*arena=aligned_alloc(32,plan.arena_bytes);if(!arena)return 1;qw_model*model=0;rc=qw_open(&model,arena,plan.arena_bytes,&o,&sources[0],&sources[1],&sources[2],&plan);if(rc){fprintf(stderr,"LOAD_ERROR=%d\n",rc);return 1;}
 qw_checkpoint_key key={0};hash(token,token_n,key.tokenizer_sha256);hash(prefix,prefix_n,key.template_sha256);hash(schema,schema_n,key.schema_sha256);key.prefix_ids=(const uint32_t*)((unsigned char*)pid_data+4);key.prefix_count=prefix_count;
 size_t need=0;if(qw_checkpoint_bytes(model,prefix_count,&need)!=QW_OK||need!=checkpoint_n){fprintf(stderr,"checkpoint length mismatch\n");return 1;}size_t work_n=qt_workspace_bytes(prefix_n);void*work=malloc(work_n);if(!work)return 1;
 ag_qw_config cfg={model,&tokenizer,prefix,prefix_n,key.prefix_ids,prefix_count,work,work_n,10,&key,checkpoint,checkpoint_n,1};ag_qw_backend backend;if(!ag_qw_backend_init(&backend,&cfg)){fprintf(stderr,"BACKEND_INIT_ERROR=%d model_rc=%d\n",backend.error,backend.model_error);return 1;}
 ag_loop loop;cap.loop=&loop;cap.backend=&backend;ag_loop_init(&loop,(ag_loop_backend){0,0},0,0,0,0);if(!ag_loop_set_backend_v2(&loop,ag_qw_backend_poll_v2,&backend)||!ag_loop_set_event_v2(&loop,on_event,&cap)||!ag_loop_set_poll_limit(&loop,0))return 1;
 fprintf(stderr,"MODEL_READY role=current_c_w4_bf16_software prefix_tokens=%u checkpoint_bytes=%zu arena_bytes=%zu\n",prefix_count,checkpoint_n,plan.arena_bytes);
 char*saveptr=0;char*line=strtok_r((char*)task_data,"\n",&saveptr);unsigned task_no=0;while(line){size_t len=strlen(line);if(len&&line[len-1]=='\r')line[len-1]=0;char*id=line,*h=strchr(id,'\t');if(!h)abort();*h++=0;char*casehash=h;h=strchr(casehash,'\t');if(!h)abort();*h++=0;char*family=h;h=strchr(family,'\t');if(!h)abort();*h++=0;char*prompt=h;if(strchr(prompt,'\t')||!*id||strlen(id)>64||strlen(casehash)!=64||strlen(prompt)>AG_LOOP_TASK_MAX)abort();if(run_one(&loop,&backend,id,casehash,family,prompt))return 1;task_no++;line=strtok_r(0,"\n",&saveptr);}
 fprintf(stderr,"MODEL_RUN_COMPLETE tasks=%u role=current_c_w4_bf16_software prefix_builds=%u\n",task_no,backend.prefix_builds);
 if(!ag_qw_backend_quiescent(&backend)||qw_close(model)!=QW_OK){fprintf(stderr,"UNKNOWN_CONTEXT_LEASE_RETAINED\n");return 1;}for(unsigned i=0;i<3;i++)fclose(fs[i].f);free(arena);free(work);free(checkpoint);free(task_data);free(schema);free(pid_data);free(prefix);free(token);return 0;
}
