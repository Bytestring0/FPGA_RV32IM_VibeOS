#include "../sw/agent/ag_agent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static ag_runtime rt;
static ag_receipt receipt;
static unsigned assertions;
#define CHECK(x) do { ++assertions; assert(x); } while (0)
static ag_code run(const char *id, const char *json, uint64_t now) {
    return ag_execute(&rt,id,json,now,&receipt);
}
static const char *calc="{\"name\":\"calculate\",\"arguments\":{\"expression\":\"12+23\"}}";
static const char *note="{\"name\":\"append_note\",\"arguments\":{\"text\":\"hello\"}}";
static const char *timer="{\"name\":\"start_timer\",\"arguments\":{\"value\":10,\"unit\":\"分钟\",\"label\":\"学习\"}}";
static int fail_save(void *ctx, const ag_state *s) { (void)ctx;(void)s;return -1; }
static ag_state disk;
static int save(void *ctx, const ag_state *s) { (void)ctx;disk=*s;return 0; }
static void test_workflow(void) {
    ag_init(&rt,0,0);
    CHECK(run("c1",calc,1000)==AG_OK); CHECK(!strcmp(receipt.result,"35"));
    CHECK(run("n1","{\"name\":\"save_result\",\"arguments\":{\"result_id\":\"c1\"}}",1000)==AG_OK);
    CHECK(rt.state.note_count==1 && !strcmp(rt.state.notes[0],"35"));
    CHECK(run("t1",timer,1000)==AG_OK); CHECK(rt.state.deadline_ms==601000);
    CHECK(run("t1",timer,2000)==AG_OK); CHECK(rt.state.deadline_ms==601000);
    CHECK(run("n1",note,2000)==AG_CONFLICT); CHECK(rt.state.note_count==1);
    CHECK(run("s1","{\"name\":\"query_state\",\"arguments\":{}}",2000)==AG_OK);
    CHECK(!strcmp(receipt.result,"notes=1;timer=1;remaining_ms=599000"));
    CHECK(ag_tick(&rt,601000)==AG_OK); CHECK(rt.state.timer==AG_FINISHED);
    uint32_t revision=rt.state.timer_revision;
    CHECK(ag_tick(&rt,602000)==AG_OK && rt.state.timer_revision==revision);
}
static void test_timer(void) {
    ag_init(&rt,0,0); CHECK(run("1",timer,0)==AG_OK);
    CHECK(run("2","{\"name\":\"pause_timer\",\"arguments\":{}}",1000)==AG_OK);
    CHECK(rt.state.timer==AG_PAUSED && rt.state.remaining_ms==599000);
    CHECK(run("3","{\"name\":\"set_timer\",\"arguments\":{\"value\":2,\"unit\":\"秒\"}}",2000)==AG_OK);
    CHECK(rt.state.timer==AG_PAUSED && rt.state.remaining_ms==2000);
    CHECK(run("4","{\"name\":\"resume_timer\",\"arguments\":{}}",3000)==AG_OK);
    CHECK(rt.state.deadline_ms==5000);
    CHECK(run("5","{\"name\":\"extend_timer\",\"arguments\":{\"value\":1.5,\"unit\":\"秒\"}}",4000)==AG_OK);
    CHECK(rt.state.deadline_ms==6500);
    CHECK(run("6",timer,5000)==AG_TIMER_STATE);
    CHECK(run("7","{\"name\":\"cancel_timer\",\"arguments\":{}}",5000)==AG_OK);
    CHECK(rt.state.timer==AG_IDLE);
    ag_init(&rt,0,0); CHECK(run("1",timer,UINT64_MAX-100)==AG_BAD_ARGS);
}
static void test_rejection(void) {
    const char *bad[]={
        "", "{}", "[]", "{\"name\":\"finish\",\"arguments\":{},\"extra\":1}",
        "{\"name\":\"finish\",\"name\":\"finish\"}",
        "{\"name\":\"append_note\",\"arguments\":{\"text\":\"a\",\"text\":\"b\"}}",
        "{\"name\":\"append_note\",\"arguments\":{\"text\":null}}",
        "{\"name\":\"append_note\",\"arguments\":{\"text\":[]}}",
        "{\"name\":\"append_note\",\"arguments\":{\"text\":\"\\u0000\"}}",
        "{\"name\":\"append_note\",\"arguments\":{\"text\":\"\\ud800\"}}",
        "{\"name\":\"append_note\",\"arguments\":{\"text\":\"\\ud800\\ uDC00\"}}",
        "{\"name\":\"finish\",\"arguments\":{}}garbage",
        "{\"name\":\"start_timer\",\"arguments\":{\"value\":01,\"unit\":\"秒\",\"label\":\"x\"}}",
        "{\"name\":\"append_note\",\"arguments\":{\"text\":\"\xc0\xaf\"}}"
    };
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        ag_init(&rt,0,0); CHECK(run("bad",bad[i],0)==AG_BAD_JSON);
        CHECK(rt.state.record_count==0 && rt.state.note_count==0);
    }
    ag_init(&rt,0,0);
    CHECK(run("x","{\"name\":\"calculate\",\"arguments\":{\"text\":\"1+1\"}}",0)==AG_BAD_ARGS);
    CHECK(run("x","{\"name\":\"flash_erase\",\"arguments\":{}}",0)==AG_UNSUPPORTED);
    CHECK(run("x","{\"name\":\"save_result\",\"arguments\":{\"result_id\":\"none\"}}",0)==AG_NOT_FOUND);
    CHECK(run("x","{\"name\":\"pause_timer\",\"arguments\":{}}",0)==AG_TIMER_STATE);
    CHECK(run("x","{\"name\":\"append_note\",\"arguments\":{\"text\":\"\\u4e2d\\u6587\\ud83d\\ude00\"}}",0)==AG_OK);
    CHECK(!strcmp(rt.state.notes[0],"中文😀"));
    CHECK(run("y","{\"arguments\":{},\"name\":\"finish\"}",0)==AG_OK);
}
static void test_storage(void) {
    ag_init(&rt,fail_save,0); CHECK(run("n",note,0)==AG_STORAGE);
    CHECK(rt.state.note_count==0 && rt.state.record_count==0);
    rt.commit=save; CHECK(run("n",note,0)==AG_OK);
    ag_init(&rt,save,0); rt.state=disk; /* host recovery fixture, not a TF disk format */
    CHECK(run("n",note,0)==AG_OK && rt.state.note_count==1);
    CHECK(run("t",timer,0)==AG_OK); rt.commit=fail_save;
    CHECK(ag_tick(&rt,600000)==AG_STORAGE && rt.state.timer==AG_RUNNING);
    rt.commit=save; CHECK(ag_tick(&rt,600000)==AG_OK && rt.state.timer==AG_FINISHED);
}
static void test_limits(void) {
    ag_init(&rt,0,0);
    for (unsigned i=0;i<AG_NOTES_MAX;++i) {
        char id[8]; sprintf(id,"n%u",i); CHECK(run(id,note,0)==AG_OK);
    }
    CHECK(run("full",note,0)==AG_LIMIT);
    CHECK(rt.state.note_count==AG_NOTES_MAX);
    ag_init(&rt,0,0);
    for (unsigned i=0;i<AG_RECEIPTS_MAX;++i) {
        char id[8]; sprintf(id,"c%u",i); CHECK(run(id,calc,0)==AG_OK);
    }
    CHECK(run("full",calc,0)==AG_LIMIT);
    CHECK(run("c0",calc,0)==AG_OK); /* full table must still allow replay */
    CHECK(run("",calc,0)==AG_BAD_ARGS);
    char large[AG_REQUEST_MAX+2]; memset(large,'x',sizeof(large)-1); large[sizeof(large)-1]=0;
    CHECK(run("a",large,0)==AG_BAD_ARGS);
}
static void test_math(void) {
    const char *cases[][2]={{"12+23","35"},{"(8+4)/3","4"},{"-2*-3","6"},
        {"1/3","0.333"},{"-1/3","-0.333"},{"0.1+0.2","0.3"},
        {"1000*1000","1000000"},{"(1.25+2.75)/2","2"},{".5+0.25","0.75"}};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        char out[AG_RESULT_MAX+1]; CHECK(ag_calculate(cases[i][0],out)==AG_OK);
        CHECK(!strcmp(out,cases[i][1]));
    }
    const char *bad[]={"1/0","1000000+1","99999999999999999999999999999","1e2","2**3","1+", "()", "0.0001", "((((((((((((((1))))))))))))))"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        char out[AG_RESULT_MAX+1]; CHECK(ag_calculate(bad[i],out)==AG_ARITHMETIC);
    }
}
static void fuzz(void) {
    /* Deterministic bounded mutation/truncation corpus, run under ASan/UBSan. */
    char buf[AG_REQUEST_MAX+1]; uint32_t seed=0x12345678;
    for (unsigned i=0;i<3000;++i) {
        size_t n=strlen(timer); memcpy(buf,timer,n+1);
        seed=seed*1664525u+1013904223u;
        if (i&1) buf[seed%n]=0;
        else buf[seed%n]=(char)(1+(seed>>16)%255);
        ag_init(&rt,0,0); (void)run("fuzz",buf,0);
    }
}
int main(int argc, char **argv) {
    if (argc==3 && !strcmp(argv[1],"--calc")) {
        char out[AG_RESULT_MAX+1]; ag_code c=ag_calculate(argv[2],out);
        puts(c==AG_OK ? out : "ERROR"); return c==AG_OK ? 0:1;
    }
    test_workflow(); test_timer(); test_rejection(); test_storage(); test_limits(); test_math(); fuzz();
    printf("Agent runtime PASS: %u assertions, 3000 parser mutations; runtime=%zu state=%zu bytes\n",assertions,sizeof(rt),sizeof(rt.state));
    return 0;
}
