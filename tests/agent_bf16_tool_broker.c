/* Host-only persistent broker for real ag_loop/ag_execute. No model or answers. */
#include "ag_loop.h"
#include "qw_json.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ag_loop loop;
static char response[AG_LOOP_RESPONSE_MAX + 1];
static char rejected_raw[2049];
static struct { unsigned kind, turn; uint64_t generation; ag_code code;
    char id[AG_ID_MAX + 1], text[AG_RESULT_MAX + 1]; } captured;

static void quoted(const char *s) {
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p) {
        if (*p=='"' || *p=='\\') { putchar('\\'); putchar(*p); }
        else if (*p<32) printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
static void state_json(const ag_state *s) {
    static const char *timers[]={"idle","running","paused","finished"};
    printf("{\"notes\":[");
    for (unsigned i=0;i<s->note_count;++i) {if(i)putchar(',');quoted(s->notes[i]);}
    printf("],\"records\":[");
    for (unsigned i=0;i<s->record_count;++i) {
        const ag_record *r=&s->records[i];if(i)putchar(',');
        printf("{\"id\":");quoted(r->id);printf(",\"tool\":");quoted(r->tool);
        printf(",\"request\":");quoted(r->request);
        printf(",\"receipt\":{\"code\":%u,\"result\":",(unsigned)r->receipt.code);
        quoted(r->receipt.result);printf("}}");
    }
    printf("],\"timer\":");quoted((unsigned)s->timer<4?timers[s->timer]:"invalid");
    printf(",\"label\":");quoted(s->label);
    printf(",\"deadline_ms\":%" PRIu64 ",\"remaining_ms\":%" PRIu64
           ",\"timer_revision\":%u,\"note_count\":%u,\"record_count\":%u}",
           s->deadline_ms,s->remaining_ms,s->timer_revision,s->note_count,s->record_count);
}
static void on_event(void *ctx,uint64_t gen,unsigned turn,ag_loop_event_kind kind,
                     const char *id,ag_code code,const char *text) {
    (void)ctx;captured.kind=kind;captured.turn=turn;captured.generation=gen;captured.code=code;
    memcpy(captured.id,id,strlen(id)+1);memcpy(captured.text,text,strlen(text)+1);
}
static ag_backend_poll_status poll_result(void *ctx,const ag_loop_input *in,uint32_t quota,
    char *out,size_t cap,size_t *n,uint64_t *gen,unsigned *turn) {
    (void)ctx;(void)quota;*n=strlen(response);*gen=in->generation;*turn=in->turn;
    if (*n>=cap) return AG_BACKEND_FAILED;
    memcpy(out,response,*n+1);return AG_BACKEND_READY;
}
static int hex_digit(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    return -1;
}
static int unhex(const char *src,char *dst,size_t cap) {
    size_t n=strlen(src);if(n%2 || n/2>=cap)return 0;
    for(size_t i=0;i<n;i+=2){int a=hex_digit(src[i]),b=hex_digit(src[i+1]);
        if(a<0||b<0||(a==0&&b==0))return 0;
        dst[i/2]=(char)(a*16+b);}
    dst[n/2]=0;return 1;
}
static int number(const char *s,uint64_t *out) {
    if(!s||!*s)return 0;
    for(const char*p=s;*p;++p)if(*p<'0'||*p>'9')return 0;
    errno=0;char *end;unsigned long long v=strtoull(s,&end,10);
    if(errno||*end)return 0;
    *out=(uint64_t)v;return 1;
}
static int request_part(char *dst,size_t cap) {
    qj_doc d={response,strlen(response)};qj_value root,v;size_t at=0;
    return !qj_read(d,&at,&root)&&at==d.n&&!qj_get(d,root,"request",&v)&&!qj_text(d,v,dst,cap);
}
static void emit_step(const ag_state *before,uint64_t now) {
    printf("{\"status\":");quoted(ag_loop_status_name(loop.status));
    printf(",\"generation\":%" PRIu64 ",\"turns\":%u,\"initial_state\":",loop.generation,loop.turns);
    state_json(before);printf(",\"final_state\":");state_json(&loop.runtime.state);
    if(rejected_raw[0]){printf(",\"rejected_raw_response\":");quoted(rejected_raw);}
    if(captured.kind){
        static const char *kinds[]={"none","tool","final","clarification","error"};
        printf(",\"event\":{\"kind\":");quoted(captured.kind<5?kinds[captured.kind]:"invalid");
        printf(",\"generation\":%" PRIu64 ",\"turn\":%u,\"id\":",captured.generation,captured.turn);
        quoted(captured.id);printf(",\"code\":%u,\"text\":",(unsigned)captured.code);quoted(captured.text);
        printf(",\"now_ms\":%" PRIu64,now);
        if(captured.kind==AG_LOOP_EVENT_TOOL){char request[AG_REQUEST_MAX+1];
            if(!request_part(request,sizeof(request)))exit(4);
            printf(",\"origin\":\"ag_execute\",\"request\":");quoted(request);
            if(captured.code==AG_OK){for(unsigned i=0;i<loop.runtime.state.record_count;++i){
                const ag_record*r=&loop.runtime.state.records[i];
                if(!strcmp(r->id,captured.id)&&!strcmp(r->tool,"calculate")){
                    printf(",\"result_id\":");quoted(r->id);break;}
            }}
        }
        putchar('}');
    }
    printf("}\n");fflush(stdout);
}
int main(void) {
    char line[8192];unsigned ready=0;uint64_t last_now=0;
    while(fgets(line,sizeof(line),stdin)) {
        size_t n=strlen(line);if(!n||line[n-1]!='\n')return 2;
        while(n&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
        char *fields[6]={line};unsigned count=1;
        for(char*p=line;*p;++p)if(*p=='\t'){*p=0;if(count==6)return 2;fields[count++]=p+1;}
        if(!strcmp(fields[0],"QUIT")&&count==1)return 0;
        rejected_raw[0]=0;
        if(!strcmp(fields[0],"RESET")&&count==4){
            uint64_t timeout,turns;char task[AG_LOOP_TASK_MAX+1];
            if(!number(fields[1],&timeout)||!timeout||timeout>UINT32_MAX||
               !number(fields[2],&turns)||!turns||turns>AG_LOOP_MAX_TURNS||
               !unhex(fields[3],task,sizeof(task)))return 2;
            ag_loop_init(&loop,(ag_loop_backend){0},0,0,0,0);
            if(!ag_loop_set_backend_v2(&loop,poll_result,0)||!ag_loop_set_event_v2(&loop,on_event,0))return 3;
            ag_state before=loop.runtime.state;memset(&captured,0,sizeof(captured));
            if(ag_loop_submit(&loop,task,0,(uint32_t)timeout,(unsigned)turns)!=AG_LOOP_RUNNING)return 3;
            ready=1;last_now=0;emit_step(&before,0);continue;
        }
        if(!ready)return 2;
        if(!strcmp(fields[0],"SNAPSHOT")&&count==1){
            memset(&captured,0,sizeof(captured));ag_state before=loop.runtime.state;emit_step(&before,last_now);continue;
        }
        uint64_t now;if(count<2||!number(fields[1],&now)||now<last_now)return 2;
        last_now=now;ag_state before=loop.runtime.state;memset(&captured,0,sizeof(captured));
        if(!strcmp(fields[0],"REJECT")&&count==3){
            if(loop.status!=AG_LOOP_RUNNING||!unhex(fields[2],rejected_raw,sizeof(rejected_raw)))return 2;
            response[0]=0;ag_loop_step(&loop,now);emit_step(&before,now);continue;
        }
        if(!strcmp(fields[0],"STEP")&&count==3){
            if(loop.status!=AG_LOOP_RUNNING||!unhex(fields[2],response,sizeof(response)))return 2;
            ag_loop_step(&loop,now);emit_step(&before,now);continue;
        }
        if(!strcmp(fields[0],"SETUP")&&count==4){
            char id[AG_ID_MAX+1],request[AG_REQUEST_MAX+1];ag_receipt receipt;
            if(loop.turns||loop.status!=AG_LOOP_RUNNING||!unhex(fields[2],id,sizeof(id))||
               !unhex(fields[3],request,sizeof(request)))return 2;
            ag_code code=ag_execute(&loop.runtime,id,request,now,&receipt);
            printf("{\"status\":\"setup\",\"generation\":%" PRIu64 ",\"initial_state\":",loop.generation);
            state_json(&before);printf(",\"final_state\":");state_json(&loop.runtime.state);
            printf(",\"event\":{\"kind\":\"tool\",\"origin\":\"ag_execute\",\"generation\":%" PRIu64 ",\"id\":",loop.generation);
            quoted(id);printf(",\"request\":");quoted(request);printf(",\"code\":%u,\"text\":",(unsigned)code);
            quoted(receipt.result);printf(",\"now_ms\":%" PRIu64 "}}\n",now);fflush(stdout);continue;
        }
        return 2;
    }
    return ferror(stdin)?2:0;
}
