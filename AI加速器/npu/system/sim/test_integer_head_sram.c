/* Directed capacity regression. Real integer-head reduction/query, synthetic
 * zero partial backend: validates every head row and tile boundary, no model claim. */
#include "qw_internal.h"
#include "qw_int_head.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls,max_rows;
static int execute(npu_raw_context*c,const npu_raw_job*j,npu_raw_budgets budgets){
 (void)budgets;npu_raw_layout l;if(npu_raw_query(j,c->config.regions,c->config.region_count,&l))return -1;
 if(l.partial_span>65536u||!j->rows||j->rows>1024u)return -1;
 unsigned char*p=c->config.mapping.base;
 for(unsigned r=0;r<j->rows;r++)for(unsigned g=0;g<l.groups;g++){
  uint32_t tag=r|(g<<16),zero=0;memcpy(p+r*l.output_stride+g*8,&tag,4);memcpy(p+r*l.output_stride+g*8+4,&zero,4);
 }
 c->result=(npu_raw_result_view){j->partial_offset,l.output_stride,j->rows,l.groups,j->global_row_base,1,0,l.expected_result_count,0};
 calls++;if(j->rows>max_rows)max_rows=j->rows;return 0;
}
int main(void){
 qw_model*m=calloc(1,sizeof(*m));uint16_t*scales=malloc(QW_VOCAB*8*sizeof(*scales));uint32_t*out=malloc(QW_VOCAB*4);unsigned char*partial=malloc(65536);int8_t*activation=malloc(4096);float input[QW_HIDDEN];
 if(!m||!scales||!out||!partial||!activation)return 2;
 for(unsigned i=0;i<QW_VOCAB*8;i++)scales[i]=0x3c00;
 for(unsigned i=0;i<QW_HIDDEN;i++)input[i]=1.0f;
 memset(out,0xa5,QW_VOCAB*4);m->magic=QW_MAGIC;m->options.tile_rows=8192;m->plan.partial_bytes=65536;m->plan.activation_bytes=4096;
 m->head_candidate_allowed=1;m->head_candidate=m->output_head=m->tensors;m->tensors[0]=(qw_tensor){0,QW_VOCAB,QW_HIDDEN,0,QT_W4,512,0x100000,0,scales};
 m->activation=activation;m->partial=partial;m->grants[0]=(npu_raw_region){0x100000,QW_VOCAB*512,NPU_RAW_WEIGHT};m->grants[1]=(npu_raw_region){0x1000,4096,NPU_RAW_ACTIVATION};m->grants[2]=(npu_raw_region){0x20000,65536,NPU_RAW_PARTIAL};
 static const npu_raw_ops ops={execute,0};m->raw.ops=&ops;m->raw.config.regions=m->grants;m->raw.config.region_count=3;m->raw.config.mapping.base=partial;
 if(qw_enable_integer_w4_head(m)||qw_integer_head_execute(m,input,out))return 3;
 for(unsigned i=0;i<QW_VOCAB;i++)if(out[i])return 4;
 if(max_rows!=1024u||calls!=(QW_VOCAB+1023u)/1024u)return 5;
 printf("INTEGER_HEAD_SRAM_CAPACITY_PASS rows=%u calls=%u max_tile=%u output_zero=true\n",QW_VOCAB,calls,max_rows);free(m);free(scales);free(out);free(partial);free(activation);return 0;
}
