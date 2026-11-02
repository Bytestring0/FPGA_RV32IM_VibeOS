/* Native reference launcher for the SAME portable C runtime, no HTTP/llama. */
#define _POSIX_C_SOURCE 200809L
#include "qw_runtime.h"
#include "qw_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
typedef struct{FILE*f;uint64_t bytes;} source;
static int read_at(void*ctx,uint64_t off,void*dst,size_t n){source*s=ctx;if(off>s->bytes||n>s->bytes-off||fseeko(s->f,(off_t)off,SEEK_SET))return -1;return fread(dst,1,n,s->f)==n?0:-1;}
static int open_source(const char*p,source*s,npu_storage_source*out){s->f=fopen(p,"rb");if(!s->f)return -1;if(fseeko(s->f,0,SEEK_END))return -1;off_t n=ftello(s->f);if(n<0)return -1;s->bytes=(uint64_t)n;*out=(npu_storage_source){read_at,s,s->bytes};return 0;}
static int hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static int digest(const char*s,unsigned char*out){if(strlen(s)!=64)return -1;for(unsigned i=0;i<32;i++){int a=hex(s[2*i]),b=hex(s[2*i+1]);if(a<0||b<0)return -1;out[i]=(unsigned char)(a*16+b);}return 0;}
static void trace(void*ctx,uint32_t pos,uint32_t layer,const char*stage,const float*p,size_t n){
 const char*dir=ctx;char path[1024];if(snprintf(path,sizeof path,"%s/p%04u_l%02u_%s.bin",dir,pos,layer,stage)>=(int)sizeof path)abort();
 FILE*f=fopen(path,"wb");if(!f||fwrite(p,sizeof(float),n,f)!=n||fclose(f))abort();
}
static void report(qw_model*m,const float*l){
 qw_status s={0};if(qw_get_status(m,&s))abort();uint32_t seed=1,token=0;if(qw_sample(l,QW_VOCAB,0,1,&seed,&token))abort();
 qw_sha256 h;unsigned char sha[32];qw_sha_init(&h);qw_sha_update(&h,l,QW_VOCAB*sizeof(float));qw_sha_final(&h,sha);
 printf("POSITION %u mode=SOFTWARE_REFERENCE jobs=%u argmax=%u logits_sha256=",s.position,s.npu_jobs,token);for(unsigned i=0;i<32;i++)printf("%02x",sha[i]);puts("");fflush(stdout);
}
int main(int argc,char**argv){
 if(argc!=10){fprintf(stderr,"usage: runtime weights cpu catalog shaW shaC shaJ tokenCSV generatedSteps outDir\n");return 2;}
 unsigned char hashes[3][32];for(unsigned i=0;i<3;i++)if(digest(argv[4+i],hashes[i]))return 2;
 source s[3];npu_storage_source src[3];for(unsigned i=0;i<3;i++)if(open_source(argv[1+i],&s[i],&src[i])){perror(argv[1+i]);return 2;}
 uint32_t tokens[1024];size_t n=0;const char*p=argv[7];while(*p){char*end;unsigned long x=strtoul(p,&end,10);if(end==p||x>=QW_VOCAB||n==1024)return 2;tokens[n++]=(uint32_t)x;if(!*end)break;if(*end!=',')return 2;p=end+1;}
 unsigned steps=(unsigned)strtoul(argv[8],0,10);if(!n||steps>1024-n)return 2;
 (void)mkdir(argv[9],0755);
 qw_options o={0};o.context_tokens=1024;o.prefill_chunk_tokens=128;o.tile_rows=512;o.arena_ddr_offset=1u<<20;o.system_reserved_bytes=1u<<20;
 o.mode=NPU_RAW_SOFTWARE_REFERENCE;o.budgets=(npu_raw_budgets){10000000,1000000};o.weight_sha256=hashes[0];o.cpu_sha256=hashes[1];o.catalog_sha256=hashes[2];o.trace=trace;o.trace_user=argv[9];
 size_t capacity=NPU_RAW_DDR_LIMIT-o.arena_ddr_offset;qw_memory_plan plan;int rc=qw_plan(&o,src[0].size_bytes,src[1].size_bytes,src[2].size_bytes,capacity,&plan);
 if(rc){fprintf(stderr,"PLAN ERROR %d\n",rc);return 1;}
 printf("PLAN arena=%zu weights=%zu cpu=%zu state=%zu workspace=%zu partial=%zu logits=%zu remaining=%zu\n",plan.arena_bytes,plan.weights_bytes,plan.cpu_bytes,plan.state_bytes,plan.work_bytes,plan.partial_bytes,plan.logits_bytes,capacity-plan.arena_bytes);fflush(stdout);
 void*arena=aligned_alloc(32,capacity);if(!arena)return 1;qw_model*m=0;clock_t began=clock();
 rc=qw_open(&m,arena,capacity,&o,&src[0],&src[1],&src[2],&plan);printf("OPEN rc=%d seconds=%.3f\n",rc,(double)(clock()-began)/CLOCKS_PER_SEC);fflush(stdout);if(rc)return 1;
 size_t processed=0;rc=qw_prefill(m,tokens,n,&processed);printf("PREFILL rc=%d processed=%zu\n",rc,processed);if(rc)return 1;
 qw_status status={0};if(qw_get_status(m,&status))return 1;const float*l=status.logits;report(m,l);
 for(unsigned i=0;i<steps;i++){uint32_t seed=1,token=0;if(qw_sample(l,QW_VOCAB,0,1,&seed,&token))return 1;printf("GENERATED token=%u stop=%d\n",token,qw_is_stop(token));fflush(stdout);if(qw_is_stop(token))break;if((rc=qw_decode(m,token,&l))){fprintf(stderr,"DECODE ERROR %d\n",rc);return 1;}report(m,l);}
 if((rc=qw_reset(m))||(rc=qw_close(m))){fprintf(stderr,"CLOSE ERROR %d\n",rc);return 1;}
 free(arena);for(unsigned i=0;i<3;i++)fclose(s[i].f);printf("FULL MODEL C REFERENCE PASS seconds=%.3f\n",(double)(clock()-began)/CLOCKS_PER_SEC);return 0;
}
