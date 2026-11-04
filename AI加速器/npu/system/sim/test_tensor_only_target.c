#include "qw_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef QW_TENSOR_ONLY_TARGET
#error "This targeted test requires explicit tensor-only profile"
#endif
static unsigned dispatched;
static int execute(void *u,const npu_tensor_job *j,npu_tensor_stats *s){(void)u;(void)j;(void)s;return 0;}
int qw_tensor_step(qw_model *m,uint32_t token,const float **out,int emit){(void)m;(void)token;(void)out;(void)emit;dispatched++;return 73;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"tensor-only failure line %d\n",__LINE__);return 1;}}while(0)
int main(void){static qw_model m;const float *out=(const float*)1;m.magic=QW_MAGIC;m.options.context_tokens=8;m.options.prefill_chunk_tokens=2;
 CHECK(qw_decode(&m,0,&out)==QW_FORMAT&&!out&&!dispatched&&!m.position);
 m.tensor_status.enabled=1;CHECK(qw_decode(&m,0,&out)==QW_FORMAT&&!dispatched);
 m.tensor_execute=execute;m.raw.config.mode=NPU_RAW_SOFTWARE_REFERENCE;CHECK(qw_decode(&m,0,&out)==QW_FORMAT&&!dispatched);
 m.raw.config.mode=NPU_RAW_HARDWARE;CHECK(qw_decode(&m,0,&out)==73&&dispatched==1);
 m.tensor_status.enabled=0;m.raw.active=1;CHECK(qw_decode(&m,0,&out)==QW_UNKNOWN&&dispatched==1);m.raw.active=0;
 m.tensor_status.unknown=1;CHECK(qw_decode(&m,0,&out)==QW_UNKNOWN&&dispatched==1);m.tensor_status.unknown=0;
 uint32_t tokens[1]={0};size_t processed=99;CHECK(qw_prefill(&m,tokens,1,&processed)==QW_FORMAT&&!processed&&dispatched==1);
 CHECK(qw_matrix(&m,0,0,0)==QW_FORMAT);
 puts("TENSOR_ONLY_TARGET_GUARD_PASS missing_tensor missing_callback software_mode_rejected hardware_dispatch unknown_lease prefill_no_fallback");return 0;}
