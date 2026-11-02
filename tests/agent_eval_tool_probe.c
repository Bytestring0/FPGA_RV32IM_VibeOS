/* Host-only probe: real ag_execute receipts/state, no model invocation. */
#include "ag_agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void quoted(const char *s) {
    putchar('"');
    for (; *s; ++s) {
        unsigned char c=(unsigned char)*s;
        if (c=='"' || c=='\\') {putchar('\\');putchar(c);}
        else if (c<32) printf("\\u%04x",c);
        else putchar(c);
    }
    putchar('"');
}
static void dump_state(const ag_state *s) {
    printf("{\"notes\":[");
    for (unsigned i=0;i<s->note_count;++i) {if(i)putchar(',');quoted(s->notes[i]);}
    printf("],\"records\":[");
    for (unsigned i=0;i<s->record_count;++i) {
        const ag_record *r=&s->records[i];if(i)putchar(',');
        printf("{\"id\":");quoted(r->id);printf(",\"tool\":");quoted(r->tool);
        printf(",\"request\":");quoted(r->request);
        printf(",\"receipt\":{\"code\":%u,\"result\":",r->receipt.code);quoted(r->receipt.result);printf("}}");
    }
    const char *timers[]={"idle","running","paused","finished"};
    printf("],\"timer\":");quoted(timers[s->timer]);printf(",\"label\":");quoted(s->label);
    printf(",\"deadline_ms\":%llu,\"remaining_ms\":%llu,\"timer_revision\":%u,\"note_count\":%u,\"record_count\":%u}",
       (unsigned long long)s->deadline_ms,(unsigned long long)s->remaining_ms,s->timer_revision,s->note_count,s->record_count);
}
int main(int argc,char **argv) {
    if(argc!=2) {fprintf(stderr,"input TSV required\n");return 2;}
    FILE *in=fopen(argv[1],"rb");if(!in){perror("open input");return 2;}
    ag_runtime rt;ag_init(&rt,0,0);char line[2048];
    while(fgets(line,sizeof(line),in)) {
        size_t n=strlen(line);while(n && (line[n-1]=='\r'||line[n-1]=='\n'))line[--n]=0;
        if(!strcmp(line,"RESET")) {ag_init(&rt,0,0);continue;}
        char *ident=strchr(line,'\t');if(!ident)return 3;*ident++=0;
        char *time=strchr(ident,'\t');if(!time)return 3;*time++=0;
        char *request=strchr(time,'\t');if(!request)return 3;*request++=0;
        unsigned long long now=strtoull(time,0,10);ag_receipt receipt;
        printf("{\"case_id\":");quoted(line);printf(",\"initial_state\":");dump_state(&rt.state);
        ag_code code=ag_execute(&rt,ident,request,(uint64_t)now,&receipt);
        printf(",\"id\":");quoted(ident);printf(",\"request\":");quoted(request);
        printf(",\"code\":%u,\"text\":",code);quoted(receipt.result);
        printf(",\"now_ms\":%llu,\"final_state\":",now);dump_state(&rt.state);printf("}\n");
    }
    fclose(in);return 0;
}
