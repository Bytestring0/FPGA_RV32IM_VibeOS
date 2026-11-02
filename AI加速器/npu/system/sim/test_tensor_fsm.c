#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../integration/agent_runtime_20261101/sw/model/qw_internal.h"
#include "../integration/agent_runtime_20261101/sw/model/qw_tensor_async.h"
static qw_model *current;static unsigned unknown_next,reentered,raw_count,max_rows,head_quant_count,head_reduce_count,quant_maximum;
qw_tensor *qw_find(qw_model*m,const char*n){(void)m;(void)n;return 0;}
float qw_value(const qw_tensor*t,uint32_t i){(void)t;(void)i;return 0;}
int qw_plan(const qw_options*o,uint64_t a,uint64_t b,uint64_t c,size_t n,qw_memory_plan*p){(void)o;(void)a;(void)b;(void)c;(void)n;(void)p;return QW_FORMAT;}
int npu_raw_recover(npu_raw_context*r,uint32_t p){(void)r;(void)p;return 0;}
int npu_raw_query(const npu_raw_job*j,const npu_raw_region*r,size_t n,npu_raw_layout*l){
 (void)r;(void)n;memset(l,0,sizeof *l);l->groups=(j->cols+127)/128;l->output_stride=(l->groups*8+31)&~31u;l->expected_result_count=j->rows*l->groups;return 0;
}
int npu_raw_execute(npu_raw_context*r,const npu_raw_job*j,npu_raw_budgets b,const npu_raw_result_view**v){
 (void)b;
 if(current->task_owner&&current->task_owner->phase==QW_TENSOR_PHASE_HEAD){
  assert(current->task_owner->head_prepared);
  for(unsigned i=0;i<current->plan.activation_bytes;i++)assert(current->activation[i]==0);
 }
 npu_raw_layout l;npu_raw_query(j,0,0,&l);assert(j->rows<=4096);if(j->rows>max_rows)max_rows=j->rows;raw_count++;
 assert(j->weight_offset==j->global_row_base*j->weight_stride);
 r->result=(npu_raw_result_view){j->partial_offset,l.output_stride,j->rows,l.groups,j->global_row_base,1,1,l.expected_result_count,0};*v=&r->result;return 0;
}
static int execute(void*u,const npu_tensor_job*j,npu_tensor_stats*s){
 (void)u;memset(s,0,sizeof *s);if(unknown_next){unknown_next=0;return QW_UNKNOWN;}
 npu_tensor_layout l;assert(!npu_tensor_query(j,&l));
  if(current->task_owner&&current->task_owner->phase==QW_TENSOR_PHASE_HEAD){
   if(j->opcode==NPU_TENSOR_QUANT_A8){head_quant_count++;s->result=quant_maximum;}
   if(j->opcode==NPU_TENSOR_REDUCE_RAW_W4){head_reduce_count++;assert(j->param0==current->task_owner->head_maximum);}
  }
 if(!reentered){reentered=1;assert(qw_reset(current)==QW_BUSY);qw_tensor_task another={0};assert(qw_tensor_task_begin(&another,current,0,0)==QW_BUSY);}
 for(uint32_t i=0;i<l.span_count;i++)if(l.spans[i].access&2){uint32_t o=l.spans[i].offset;unsigned char*p=o>=current->tensor_ddr?(unsigned char*)current->tensor_scratch+o-current->tensor_ddr:current->arena+o;memset(p,0,l.spans[i].bytes);}
 return 0;
}
int main(void){
 enum{ARENA=32*1024*1024};unsigned char*arena=aligned_alloc(32,ARENA);unsigned char*scratch=aligned_alloc(32,qw_tensor_workspace_bytes());assert(arena&&scratch);memset(arena,0,ARENA);
 qw_model*m=calloc(1,sizeof *m);assert(m);current=m;m->magic=QW_MAGIC;m->arena=arena;m->arena_capacity=ARENA;m->plan.arena_bytes=ARENA;m->plan.state_bytes=20*1024*1024;
 m->delta=(float*)arena;m->conv=(float*)(arena+19*1024*1024);m->kv=(float*)(arena+21*1024*1024);m->activation=(int8_t*)(arena+23*1024*1024);m->partial=arena+24*1024*1024;m->logits=(float*)(arena+26*1024*1024);m->w=calloc(1,sizeof *m->w);assert(m->w);
 m->plan.activation_bytes=4096;m->plan.partial_bytes=1024*1024;m->grants[1].offset=23*1024*1024;m->grants[2].offset=24*1024*1024;m->options.context_tokens=40;m->options.tile_rows=8192;m->options.prefill_chunk_tokens=128;
 qw_tensor norm={.dtype=QT_BF16,.data=arena+30*1024*1024},small={.dtype=QT_W4,.n=16,.k=1024,.stride=512,.scales=(uint16_t*)(arena+4*1024*1024)};
 qw_tensor qkv=small,z=small,proj=small,gate=small,down=small,q=small,k=small,head=small;
 qkv.n=6144;z.n=2048;proj.n=1024;proj.k=2048;gate.n=3584;down.n=1024;down.k=3584;q.n=4096;k.n=512;head.n=QW_VOCAB;
 m->embedding=&norm;m->final_norm=&norm;m->head_candidate=m->output_head=&head;
 for(unsigned i=0;i<24;i++)m->layers[i]=(qw_layer){.norm=&norm,.post=&norm,.gate=&gate,.up=&gate,.down=&down,.qkv=&qkv,.z=&z,.a=&small,.b=&small,.conv=&norm,.dt=&norm,.al=&norm,.lnorm=&norm,.out=&proj,.q=&q,.k=&k,.v=&k,.qn=&norm,.kn=&norm};
 assert(qw_tensor_enable(m,execute,0,scratch,qw_tensor_workspace_bytes(),ARENA)==0);
 qw_tensor_task task={0},copy;const float*logits=0;assert(qw_tensor_task_begin((qw_tensor_task*)(arena+128),m,0,1)==QW_ARGUMENT);
 assert(qw_tensor_task_begin(&task,m,0,1)==0);qw_model second=*m;second.task_active=0;second.task_owner=0;assert(qw_tensor_task_begin(&task,&second,0,0)==QW_BUSY&&task.model==m);assert(qw_reset(m)==QW_BUSY&&qw_close(m)==QW_BUSY);
 assert(qw_decode(m,0,&logits)==QW_BUSY);size_t processed=9;uint32_t token=0;assert(qw_prefill(m,&token,1,&processed)==QW_BUSY);
 copy=task;assert(qw_tensor_task_advance(&copy,1,&logits)==QW_ARGUMENT);
 assert(qw_tensor_task_advance(&task,0,&logits)==QW_ARGUMENT&&task.work_units==0);
 int rc=qw_tensor_task_advance(&task,1,&logits);assert(rc==QW_PENDING&&task.phase==QW_TENSOR_PHASE_LAYER&&m->position==0);
 while(task.phase!=QW_TENSOR_PHASE_HEAD){unsigned prior=task.work_units;assert(qw_tensor_task_advance(&task,1,&logits)==QW_PENDING);assert(task.work_units==prior+1&&m->position==0);}
 unsigned tiles=0;while(task.phase==QW_TENSOR_PHASE_HEAD){unsigned prior=task.head_row;assert(qw_tensor_task_advance(&task,1,&logits)==QW_PENDING);assert(task.head_row-prior<=4096&&task.head_row>prior);tiles++;assert(m->position==0);}
 assert(tiles==(QW_VOCAB+4095)/4096&&task.phase==QW_TENSOR_PHASE_COMMIT);
  assert(head_quant_count==1&&head_reduce_count==tiles&&task.head_prepared&&task.head_maximum==0);
 assert(qw_tensor_task_advance(&task,1,&logits)==0&&m->position==1&&m->tensor_status.tokens==1&&!m->task_active&&logits==m->logits);
  assert(!task.head_prepared&&!task.head_maximum);
 assert(qw_tensor_task_advance(&task,1,&logits)==QW_ARGUMENT&&m->position==1);assert(max_rows==4096);
 assert(qw_tensor_task_begin(&task,m,0,0)==0);assert(qw_tensor_task_advance(&task,1,&logits)==QW_PENDING);unknown_next=1;
 assert(qw_tensor_task_advance(&task,1,&logits)==QW_UNKNOWN&&m->needs_reset&&m->task_active&&task.state==QW_TENSOR_TASK_UNKNOWN);
 assert(qw_tensor_task_discard(&task)==QW_UNKNOWN&&qw_reset(m)==QW_UNKNOWN);assert(qw_tensor_acknowledge_recovery(m)==0);assert(qw_tensor_task_discard(&task)==0&&!m->task_active&&m->needs_reset);m->needs_reset=0;m->position=0;
 assert(qw_tensor_task_begin(&task,m,0,0)==0);assert(qw_tensor_task_discard(&task)==0&&m->needs_reset&&m->position==0);
 m->needs_reset=0;m->position=0;quant_maximum=65536;head_quant_count=0;head_reduce_count=0;
  assert(qw_tensor_task_begin(&task,m,0,1)==0&&!task.head_prepared);
  while(task.phase!=QW_TENSOR_PHASE_HEAD)assert(qw_tensor_task_advance(&task,1,&logits)==QW_PENDING);
  assert(qw_tensor_task_advance(&task,1,&logits)==QW_PENDING&&task.head_prepared&&task.head_maximum==65536);
  assert(head_quant_count==1&&head_reduce_count==1);unknown_next=1;
  assert(qw_tensor_task_advance(&task,1,&logits)==QW_UNKNOWN&&task.head_prepared&&task.head_maximum==65536&&m->task_active);
  assert(head_quant_count==1&&qw_tensor_task_discard(&task)==QW_UNKNOWN);
  assert(qw_tensor_acknowledge_recovery(m)==0&&qw_tensor_task_discard(&task)==0&&!task.head_prepared&&!task.head_maximum);
  printf("HEAD_QUANT_REUSE head_tiles=%u quant_jobs=1 saved_quant_jobs=%u zero_valid=1 unknown_retains_prepared=1\n",tiles,tiles-1);
  free(m->w);free(m);free(scratch);free(arena);puts("PASS_TENSOR_FSM quota phases head_tiles commit_once owner reentry unknown discard head_quant_reuse");return 0;
}
