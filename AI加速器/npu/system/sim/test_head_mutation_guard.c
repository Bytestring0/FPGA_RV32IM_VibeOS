#include "qw_internal.h"
#include "qw_head.h"
#include "qw_int_head.h"
#include <assert.h>
#include <stdio.h>
int npu_raw_query(const npu_raw_job *job,const npu_raw_region *regions,size_t count,npu_raw_layout *layout){(void)job;(void)regions;(void)count;(void)layout;return 1;}
int main(void){
 qw_model m={0};m.magic=QW_MAGIC;
 m.task_active=1;
 assert(qw_enable_w4_head(&m,QW_W4_HEAD_TENSOR_NAME)==QW_BUSY);
 assert(qw_enable_integer_w4_head(&m)==QW_BUSY);
 m.task_active=0;m.inside_step=1;
 assert(qw_enable_w4_head(&m,QW_W4_HEAD_TENSOR_NAME)==QW_BUSY);
 assert(qw_enable_integer_w4_head(&m)==QW_BUSY);
 m.inside_step=0;m.tensor_status.unknown=1;
 assert(qw_enable_w4_head(&m,QW_W4_HEAD_TENSOR_NAME)==QW_UNKNOWN);
 assert(qw_enable_integer_w4_head(&m)==QW_UNKNOWN);
 m.tensor_status.unknown=0;m.raw.active=1;m.needs_reset=1;
 assert(qw_enable_w4_head(&m,QW_W4_HEAD_TENSOR_NAME)==QW_UNKNOWN);
 assert(qw_enable_integer_w4_head(&m)==QW_UNKNOWN);
 m.raw.active=0;m.needs_reset=1;
 assert(qw_enable_w4_head(&m,QW_W4_HEAD_TENSOR_NAME)==QW_NEED_RESET);
 assert(qw_enable_integer_w4_head(&m)==QW_NEED_RESET);
 m.needs_reset=0;
 assert(qw_enable_w4_head(&m,"wrong.name")==QW_ARGUMENT);
 puts("PASS_HEAD_MUTATION_GUARD busy unknown precedence needs-reset argument");
 return 0;
}
