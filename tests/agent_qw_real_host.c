/* Native software-only R4/Qwen tool/receipt integration: no scripted generation.
 * Existing W4/BF16 resources are opened in place. PASS requires actual C tools. */
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

typedef struct{FILE*f;uint64_t bytes,bytes_read;} source;
static int read_at(void*ctx,uint64_t at,void*dst,size_t n){source*s=(source*)ctx;
 if(at>s->bytes||n>s->bytes-at||fseeko(s->f,(off_t)at,SEEK_SET))return -1;
 if(fread(dst,1,n,s->f)!=n)return -1;s->bytes_read+=n;return 0;
}
static npu_storage_source open_source(const char*path,source*s){
 s->f=fopen(path,"rb");if(!s->f||fseeko(s->f,0,SEEK_END))abort();
 off_t n=ftello(s->f);if(n<0)abort();s->bytes=(uint64_t)n;s->bytes_read=0;
 return (npu_storage_source){read_at,s,s->bytes};
}
static void *read_file(const char*path,size_t*size){FILE*f=fopen(path,"rb");if(!f||fseeko(f,0,SEEK_END))abort();
 off_t n=ftello(f);if(n<0||fseeko(f,0,SEEK_SET))abort();
 void *data=malloc((size_t)n+1);if(!data||fread(data,1,(size_t)n,f)!=(size_t)n||fclose(f))abort();
 ((char*)data)[n]=0;*size=(size_t)n;return data;
}
static uint64_t clock_ms(void*ignored){(void)ignored;struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return(uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void sha(const void*p,size_t n,uint8_t out[32]){qw_sha256 h;qw_sha_init(&h);qw_sha_update(&h,p,n);qw_sha_final(&h,out);}
static int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static void parse_sha(const char*p,uint8_t out[32]){assert(strlen(p)==64);for(unsigned i=0;i<32;i++){int a=nibble(p[2*i]),b=nibble(p[2*i+1]);assert(a>=0&&b>=0);out[i]=(uint8_t)(a*16+b);}}
static struct{ag_loop*loop;ag_qw_backend*backend;unsigned tool_count,success_count;} events;
static void event(void*ctx,ag_loop_event_kind kind,const char*id,ag_code code,const char*text){
 (void)ctx;printf("EVENT generation=%" PRIu64 " turn=%u kind=%d id=%s code=%s text=%s\n",
 events.loop->generation,events.loop->turns,(int)kind,id,ag_code_name(code),text);
 if(kind==AG_LOOP_EVENT_TOOL){events.tool_count++;if(code==AG_OK)events.success_count++;}
 if(kind==AG_LOOP_EVENT_TOOL||kind==AG_LOOP_EVENT_ERROR){
  ag_qw_backend*b=events.backend;
  printf("BACKEND_DIAGNOSTIC error=%d model_rc=%d last_token=%u flags=%u generated=%u raw_bytes=%zu utf8_left=%u\n",b->error,b->model_error,b->last_generated_token,b->last_piece_flags,b->generated_tokens,b->raw_len,b->utf8_left);
  printf("RAW_MODEL_OUTPUT_BEGIN\n%s\nRAW_MODEL_OUTPUT_END\n",b->raw);
 }fflush(stdout);
}
static int run_task(ag_loop*loop,ag_qw_backend*b,const char*task,uint32_t timeout){
 uint64_t at=clock_ms(0);if(ag_loop_submit(loop,task,at,timeout,AG_LOOP_MAX_TURNS)!=AG_LOOP_RUNNING)return 2;
 uint64_t start=at,next=at+1000;
 while(loop->status==AG_LOOP_RUNNING){at=clock_ms(0);(void)ag_tick(&loop->runtime,at);
  ag_loop_step(loop,at);
  if(at>=next){qw_status s;if(!qw_get_status(b->model,&s))
    printf("PROGRESS elapsed_ms=%" PRIu64 " model_pos=%u layer=%u stage=%u completed=%zu checkpoint_hit=%u builds=%u\n",at-start,s.position,s.layer,s.stage,s.completed_tokens,b->prefix_hit,b->prefix_builds);
   fflush(stdout);next=at+1000;}
 }
 printf("TASK_END generation=%" PRIu64 " status=%s elapsed_ms=%" PRIu64 " tools=%u success_receipts=%u checkpoint_hit=%u checkpoint_builds=%u\n",
  loop->generation,ag_loop_status_name(loop->status),clock_ms(0)-start,events.tool_count,events.success_count,b->prefix_hit,b->prefix_builds);fflush(stdout);
 if(loop->awaiting_quiescence){if(ag_qw_backend_abort(b,loop->generation)&&
    ag_qw_backend_quiescent(b))assert(ag_loop_ack_quiescent(loop,loop->generation));
  else fprintf(stderr,"UNKNOWN_LEASE_RETAINED generation=%" PRIu64 "\n",loop->generation);}
 return loop->status==AG_LOOP_FINISHED?0:1;
}
static int argument_text(const ag_record*r,const char*name,char*out,size_t cap){
 qj_doc d={r->request,strlen(r->request)};qj_value root,args,v;size_t at=0;
 return !qj_read(d,&at,&root)&&at==d.n&&!qj_get(d,root,"arguments",&args)&&
  !qj_get(d,args,name,&v)&&!qj_text(d,v,out,cap);
}
static int calculation(const ag_record*r,const char*expression,const char*result){
 if(strcmp(r->tool,"calculate")||r->receipt.code!=AG_OK||strcmp(r->receipt.result,result))return 0;
 char raw[AG_TEXT_MAX+1],compact[AG_TEXT_MAX+1];if(!argument_text(r,"expression",raw,sizeof raw))return 0;
 size_t n=0;for(size_t i=0;raw[i];i++)if(raw[i]!=' '&&raw[i]!='\t'&&raw[i]!='\r'&&raw[i]!='\n')compact[n++]=raw[i];compact[n]=0;
 return !strcmp(compact,expression);
}
static int saved_calculation(const ag_state*s,const char*expression,const char*result){
 const ag_record*calc=0;unsigned count=0;
 for(unsigned i=0;i<s->record_count;i++)if(calculation(&s->records[i],expression,result)){calc=&s->records[i];count++;}
 if(count!=1||!calc||s->note_count!=1||strcmp(s->notes[0],result))return 0;
 unsigned saves=0;for(unsigned i=0;i<s->record_count;i++)if(!strcmp(s->records[i].tool,"save_result")&&s->records[i].receipt.code==AG_OK){
  char ref[AG_ID_MAX+1];if(!argument_text(&s->records[i],"result_id",ref,sizeof ref)||strcmp(ref,calc->id))return 0;saves++;
 }return saves==1;
}
int main(int argc,char**argv){
 if(argc!=9){fprintf(stderr,"usage: agent_qw_real_host W4 CPU_RAW TENSOR.JSON TOKEN.BIN R4.txt R4_IDS.bin tools.openai.json task\n");return 2;}
 const char*expected[3]={"f2299b56becb55c0f4bf06e38d982055b893c0d8e70d369f97f84fab68ef4141",
 "168f02aee67afee5b56edb8683c98f5fa97dae5c48dad720984a4c1ff2e5262d",
 "400952e58eb87cc2da45e46d3adfd66035473771eb584fff993e9f17083f0f3d"};
 source files[3];npu_storage_source sources[3];uint8_t ids[3][32];
 for(unsigned i=0;i<3;i++){sources[i]=open_source(argv[i+1],&files[i]);parse_sha(expected[i],ids[i]);}
 size_t token_bytes,prefix_bytes,id_bytes,schema_bytes;
 void*token_data=read_file(argv[4],&token_bytes),*prefix=read_file(argv[5],&prefix_bytes);
 void*binary_ids=read_file(argv[6],&id_bytes),*schema=read_file(argv[7],&schema_bytes);
 assert(id_bytes>=8);uint32_t token_count;memcpy(&token_count,binary_ids,4);
 assert(token_count>0&&token_count<QW_CONTEXT_MAX&&id_bytes==4+(size_t)token_count*4&&
        prefix_bytes>0&&prefix_bytes<=AG_QW_PREFIX_UTF8_MAX);
 uint8_t tokenizer_sha[32];parse_sha("66df043b96ef2ccaa8d6e24148b0dc03eb022e8f461a88fdc527c178cbe4aa45",tokenizer_sha);
 qw_tokenizer tokenizer;assert(qt_open(&tokenizer,token_data,token_bytes,tokenizer_sha)==QW_OK);
 qw_options o={0};o.context_tokens=2048;o.prefill_chunk_tokens=128;o.tile_rows=512;
 o.arena_ddr_offset=1u<<20;o.system_reserved_bytes=1u<<20;o.mode=NPU_RAW_SOFTWARE_REFERENCE;
 o.budgets=(npu_raw_budgets){10000000,1000000};o.weight_sha256=ids[0];o.cpu_sha256=ids[1];o.catalog_sha256=ids[2];o.now_ms=clock_ms;
 size_t cap=NPU_RAW_DDR_LIMIT-o.arena_ddr_offset;qw_memory_plan plan;int rc=qw_plan(&o,sources[0].size_bytes,sources[1].size_bytes,sources[2].size_bytes,cap,&plan);
 if(rc){fprintf(stderr,"PLAN_ERROR=%d\n",rc);return 1;}
 void*arena=aligned_alloc(32,plan.arena_bytes);if(!arena)return 1;qw_model*model=0;
 rc=qw_open(&model,arena,plan.arena_bytes,&o,&sources[0],&sources[1],&sources[2],&plan);
 if(rc){fprintf(stderr,"LOAD_ERROR=%d\n",rc);return 1;}
 qw_checkpoint_key key={0};sha(token_data,token_bytes,key.tokenizer_sha256);
 sha(prefix,prefix_bytes,key.template_sha256);sha(schema,schema_bytes,key.schema_sha256);
 key.prefix_ids=(const uint32_t*)((unsigned char*)binary_ids+4);key.prefix_count=token_count;
 size_t snap_bytes;assert(qw_checkpoint_bytes(model,token_count,&snap_bytes)==QW_OK);
 void*snap=malloc(snap_bytes);assert(snap);
 const char*checkpoint_path=getenv("PREFIX_CHECKPOINT_FILE");unsigned checkpoint_candidate=0;
 if(checkpoint_path){FILE*in=fopen(checkpoint_path,"rb");
  if(in){const char*expected_image=getenv("PREFIX_CHECKPOINT_SHA256");
   if(!expected_image||fseeko(in,0,SEEK_END)||ftello(in)!=(off_t)snap_bytes){fprintf(stderr,"CHECKPOINT_SOURCE_IDENTITY_REQUIRED\n");return 1;}
   source cache={in,snap_bytes,0};npu_storage_source cache_source={read_at,&cache,snap_bytes};uint8_t image_sha[32];parse_sha(expected_image,image_sha);
   int loaded=qw_checkpoint_restore_from_source(model,&key,&cache_source,image_sha,snap,snap_bytes);
   if(fclose(in)||loaded){fprintf(stderr,"CHECKPOINT_SOURCE_RESTORE_ERROR=%d\n",loaded);return 1;}
   printf("CHECKPOINT_EXACT_SOURCE_RESTORED bytes=%" PRIu64 " position=%zu\n",cache.bytes_read,key.prefix_count);
   if(getenv("CHECKPOINT_VALIDATE_ONLY")){
    qw_status state;if(qw_get_status(model,&state)||state.position!=key.prefix_count||state.lease_active||state.execution_active){fprintf(stderr,"REAL_SOURCE_RESTORE_STATE_ERROR\n");return 1;}
    printf("REAL_OFFLINE_CHECKPOINT_SOURCE_PASS bytes=%" PRIu64 " position=%u\n",cache.bytes_read,state.position);
    if(qw_close(model))return 1;for(unsigned i=0;i<3;i++)fclose(files[i].f);
    free(snap);free(arena);free(binary_ids);free(schema);free(prefix);free(token_data);return 0;
   }
   if(qw_reset(model)){fprintf(stderr,"CHECKPOINT_RESET_ERROR_LEASE_RETAINED\n");return 1;}
   checkpoint_candidate=1;
  }else if(errno!=ENOENT){perror("CHECKPOINT_READ_ERROR");return 1;}
 }
 size_t workspace_bytes=qt_workspace_bytes(prefix_bytes);void*workspace=malloc(workspace_bytes);assert(workspace);
 ag_qw_backend backend;ag_qw_config config={model,&tokenizer,prefix,prefix_bytes,key.prefix_ids,token_count,
  workspace,workspace_bytes,10,&key,snap,snap_bytes,checkpoint_candidate};
 if(!ag_qw_backend_init(&backend,&config)){fprintf(stderr,"BACKEND_INIT_ERROR=%d model_rc=%d\n",backend.error,backend.model_error);return 1;}
 ag_loop loop;events.loop=&loop;events.backend=&backend;ag_loop_init(&loop,(ag_loop_backend){0,0},event,0,0,0);
 assert(ag_loop_set_backend_v2(&loop,ag_qw_backend_poll_v2,&backend));assert(ag_loop_set_poll_limit(&loop,0));
 uint64_t warm_bytes=files[0].bytes_read+files[1].bytes_read+files[2].bytes_read;
 printf("MODEL_READY arena=%zu checkpoint=%zu tokenizer=%zu workspace=%zu peak_lower_bound=%zu source_bytes_loaded=%" PRIu64 "\n",
  plan.arena_bytes,snap_bytes,token_bytes,workspace_bytes,plan.arena_bytes+snap_bytes+token_bytes+workspace_bytes,warm_bytes);fflush(stdout);
 int first=run_task(&loop,&backend,argv[8],7200000u);
 if(checkpoint_path&&backend.checkpoint_ready&&(!checkpoint_candidate||backend.prefix_builds)){
  FILE*out=fopen(checkpoint_path,"wb");
  if(!out||fwrite(snap,1,snap_bytes,out)!=snap_bytes||fclose(out)){fprintf(stderr,"CHECKPOINT_WRITE_ERROR\n");return 1;}
  uint8_t digest[32];sha(snap,snap_bytes,digest);
  printf("CHECKPOINT_PERSISTED bytes=%zu sha256=",snap_bytes);for(unsigned i=0;i<32;i++)printf("%02x",digest[i]);puts("");fflush(stdout);
 }
 unsigned first_semantic=first==0&&saved_calculation(&loop.runtime.state,"19+23","42");
 unsigned before_records=loop.runtime.state.record_count,before_notes=loop.runtime.state.note_count;
 ag_timer_status before_timer=loop.runtime.state.timer;uint32_t before_revision=loop.runtime.state.timer_revision;
 printf("FIRST_TASK_SEMANTIC_OK=%u records=%u notes=%u\n",first_semantic,before_records,before_notes);fflush(stdout);
 int second=2;if(!loop.awaiting_quiescence)second=run_task(&loop,&backend,"Calculate 7*8",600000u);
 unsigned second_calc=0;for(unsigned i=before_records;i<loop.runtime.state.record_count;i++)
  if(calculation(&loop.runtime.state.records[i],"7*8","56"))second_calc++;
 unsigned second_semantic=second==0&&second_calc==1&&loop.runtime.state.note_count==before_notes&&
  loop.runtime.state.timer==before_timer&&loop.runtime.state.timer_revision==before_revision;
 printf("SECOND_TASK_SEMANTIC_OK=%u new_calculations=%u\n",second_semantic,second_calc);fflush(stdout);
 uint64_t after=files[0].bytes_read+files[1].bytes_read+files[2].bytes_read;
 printf("MODEL_RESOURCE_WARM_READ_BYTES=%" PRIu64 " first=%d second=%d cache_hits=%u builds=%u\n",after-warm_bytes,first,second,backend.prefix_hit,backend.prefix_builds);fflush(stdout);
 if(ag_qw_backend_quiescent(&backend)&&!qw_close(model)){free(snap);free(workspace);free(arena);}else fprintf(stderr,"UNKNOWN_CONTEXT_LEASE_RETAINED\n");
 for(unsigned i=0;i<3;i++)fclose(files[i].f);free(binary_ids);free(schema);free(prefix);free(token_data);
 return first_semantic&&second_semantic&&after==warm_bytes?0:1;
}
