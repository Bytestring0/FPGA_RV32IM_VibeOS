#include "qw_internal.h"
#include "qw_math.h"
#include "qw_json.h"
#include "qw_sha256.h"
#include "vendor/npu_v1/npu_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int unknown(npu_raw_context*c,uint32_t n){(void)c;(void)n;return NPU_RAW_UNKNOWN;}
static int quiet(npu_raw_context*c,uint32_t n){(void)n;c->active=0;return 0;}
int main(void){
 qw_sha256 sha;unsigned char h[32];static const unsigned char expected[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
 qw_sha_init(&sha);qw_sha_update(&sha,"a",1);qw_sha_update(&sha,"bc",2);qw_sha_final(&sha,h);assert(!memcmp(h,expected,32));
 const char*js="{\"n\":18446744073709551615,\"a\":[1,2,{\"k\":\"v\"}],\"x\":null}";qj_doc d={js,strlen(js)};qj_value root,v;size_t p=0;uint64_t u;assert(!qj_read(d,&p,&root)&&p==d.n);assert(!qj_get(d,root,"n",&v)&&!qj_uint(d,v,&u)&&u==UINT64_MAX);assert(qj_get(d,root,"none",&v)==1);
 const char*bad[]={"{\"a\":1,}","[1,]","{\"a\" 1}","[01]","[truefalse]","\"\\u00xx\"","{\"a\":18446744073709551616}"};
 for(unsigned i=0;i<6;i++){d=(qj_doc){bad[i],strlen(bad[i])};p=0;assert(qj_read(d,&p,&v));}
 d=(qj_doc){bad[6],strlen(bad[6])};p=0;assert(!qj_read(d,&p,&root)&&!qj_get(d,root,"a",&v)&&qj_uint(d,v,&u));
 qw_options o={0};o.context_tokens=1024;o.prefill_chunk_tokens=128;o.tile_rows=512;o.arena_ddr_offset=1048576;o.system_reserved_bytes=1048576;
 qw_memory_plan plan;assert(!qw_plan(&o,256604672,509561536,600000,1072693248,&plan));assert(plan.arena_bytes<1072693248&&plan.state_bytes>40000000);assert(qw_plan(&o,256604672,509561536,600000,100,&plan)==QW_CAPACITY);o.context_tokens=128;assert(!qw_plan(&o,256604672,509561536,600000,1072693248,&plan));assert(plan.context_tokens==128);o.context_tokens=2048;assert(!qw_plan(&o,256604672,509561536,600000,1072693248,&plan));assert(plan.state_bytes==(size_t)(18u*16u*128u*128u+18u*6144u*3u+6u*2048u*1024u)*sizeof(float));assert(sizeof(((qw_work*)0)->scores)/sizeof(float)==2048);o.context_tokens=QW_CONTEXT_MAX+1;assert(qw_plan(&o,1,1,1,100,&plan)==QW_ARGUMENT);
 float x[]={0.5f,1.5f,2.5f,-0.5f,-1.5f,-2.5f,127.0f};int8_t a[7];float scale;assert(!npu_quantize_activation(x,7,a,&scale)&&scale==1&&a[0]==0&&a[1]==2&&a[2]==2&&a[3]==0&&a[4]==-2&&a[5]==-2);
 unsigned char records[64]={0};records[4]=3;records[10]=1;records[12]=7;uint16_t sw[2]={0x3c00,0x3800};float output;
 assert(!qw_reduce(records,64,1,2,32,sw,0.25f,&output)&&output==1.625f);records[2]=1;assert(qw_reduce(records,64,1,2,32,sw,0.25f,&output));
 float state[4]={0},q[2]={1,0},k[2]={1,0},val[2]={2,4},out[2];qw_delta(state,q,k,val,1,0.5f,out,2);assert(fabsf(out[0]-0.7071061f)<2e-6f&&fabsf(out[1]-1.4142122f)<3e-6f);
 qw_model*m=calloc(1,sizeof *m);assert(m);m->magic=QW_MAGIC;m->raw.active=1;static const npu_raw_ops blocked={0,unknown},recovered={0,quiet};m->raw.ops=&blocked;
 assert(qw_close(m)==QW_UNKNOWN&&m->raw.active==1&&m->magic==QW_MAGIC);m->raw.ops=&recovered;assert(!qw_close(m)&&!m->magic);free(m);
 float logits[]={1,5,5,-3};uint32_t seed=1,token;assert(!qw_sample(logits,4,0,1,&seed,&token)&&token==1);assert(qw_is_stop(248046)&&!qw_is_stop(42));
 puts("MODEL RUNTIME UNIT PASS: JSON/SHA256/arena/RNE/reduction/DeltaNet/UNKNOWN lease/sampling");return 0;
}
