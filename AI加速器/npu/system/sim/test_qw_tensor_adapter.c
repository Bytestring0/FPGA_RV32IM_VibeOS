/* Concentrated adapter boundaries. The matrix transport is a zero-output
 * mock; tensor arithmetic is the real software reference. No model-quality
 * or hardware claim follows from this test. Compile with QW_ENABLE_TENSOR_V2. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../integration/agent_runtime_20261101/sw/model/qw_tensor_v2.c"

static qw_model *current;
static npu_tensor_mapping mapping;
static unsigned unknown_next,mat32,mat4,vec32,vec4,soft36;
static uint64_t ticks;
static uint64_t clock_tick(void *unused){(void)unused;return ++ticks;}
qw_tensor *qw_find(qw_model *m,const char *name){(void)m;(void)name;return 0;}
int npu_raw_recover(npu_raw_context *raw,uint32_t polls){(void)raw;(void)polls;return NPU_RAW_UNKNOWN;}
int npu_raw_query(const npu_raw_job *j,const npu_raw_region *r,size_t n,npu_raw_layout *l){
 (void)r;(void)n;memset(l,0,sizeof *l);l->groups=(j->cols+127)/128;
 l->output_stride=(l->groups*8+31)&~31u;l->expected_result_count=j->rows*l->groups;return 0;
}
int npu_raw_execute(npu_raw_context *raw,const npu_raw_job *j,npu_raw_budgets budgets,const npu_raw_result_view **view){
 (void)budgets;npu_raw_layout l;assert(!npu_raw_query(j,0,0,&l));
 /* QUANT_A8 writes only real K; adapter must clear all raw padded lanes. */
 for(uint32_t i=j->cols;i<current->plan.activation_bytes;i++)assert(current->activation[i]==0);
 for(uint32_t r=0;r<j->rows;r++)for(uint32_t g=0;g<l.groups;g++){
  unsigned char *p=current->partial+(size_t)r*l.output_stride+g*8;
  p[0]=(unsigned char)r;p[1]=(unsigned char)(r>>8);p[2]=(unsigned char)g;p[3]=(unsigned char)(g>>8);memset(p+4,0,4);
 }
 raw->result=(npu_raw_result_view){j->partial_offset,l.output_stride,j->rows,l.groups,j->global_row_base,1,77,l.expected_result_count,0};
 *view=&raw->result;return 0;
}
static int execute(void *unused,const npu_tensor_job *j,npu_tensor_stats *s){
 (void)unused;if(unknown_next){unknown_next=0;return QW_UNKNOWN;}
 if(j->opcode==NPU_TENSOR_MATVEC_Q16){if(j->n==32)mat32++;if(j->n==4)mat4++;}
 if(j->opcode==NPU_TENSOR_VECMAT_Q16){if(j->n==32)vec32++;if(j->n==4)vec4++;}
 if(j->opcode==NPU_TENSOR_SOFTMAX&&j->n==36)soft36++;
 return npu_tensor_reference(j,&mapping,s)?QW_DEVICE:0;
}
int main(void){
 enum {CONTEXT=40,STATE_BYTES=CONTEXT*1024*4,SCALE_BASE=STATE_BYTES+4096+4096,ARENA_BYTES=SCALE_BASE+65536};
 size_t bytes=ARENA_BYTES+WS_BYTES;unsigned char *memory=aligned_alloc(32,bytes);assert(memory);memset(memory,0,bytes);
 qw_model m={0};qw_tensor head={0},q={0},k={0},v={0},out={0},qn={0};
 unsigned char norm_data[256*2]={0};uint16_t *scales=(uint16_t*)(memory+SCALE_BASE);
 for(unsigned i=0;i<4096*8;i++)scales[i]=0x3c00;
 head.dtype=QT_W4;m.magic=QW_MAGIC;m.head_candidate=m.output_head=&head;
 m.arena=memory;m.plan.arena_bytes=ARENA_BYTES;m.plan.state_bytes=STATE_BYTES;m.plan.activation_bytes=4096;m.plan.partial_bytes=4096;
 m.options.context_tokens=CONTEXT;m.options.tile_rows=32;m.options.mode=NPU_RAW_HARDWARE;
 m.kv=(float*)memory;m.activation=(int8_t*)(memory+STATE_BYTES);m.partial=memory+STATE_BYTES+4096;
 m.grants[1].offset=STATE_BYTES;m.grants[2].offset=STATE_BYTES+4096;
 m.w=calloc(1,sizeof *m.w);assert(m.w);current=&m;
 mapping=(npu_tensor_mapping){memory,bytes,0};
 assert(qw_tensor_enable(&m,execute,0,memory+ARENA_BYTES,WS_BYTES,ARENA_BYTES)==0);
 assert(pack_q16(-65536)==0xbf800000u&&pack_q16(INT32_MIN)==0xc7000000u);

 /* A successful RAW launch is counted even if subsequent reduction fails. */
 qw_profile profile={0};m.options.profile=&profile;m.options.profile_clock=clock_tick;
 qw_tensor odd={0};odd.dtype=QT_W4;odd.n=1;odd.k=129;odd.stride=128;odd.scales=scales;
 int32_t input[129]={0},output[1];memset(m.activation,0x55,4096);input[0]=-65536;
 assert(matrix(&m,&odd,input,output,0)==0&&output[0]==0);
 assert(profile.matrix_calls==1&&profile.jobs_started==1&&profile.jobs_completed==1);
 assert(profile.hardwarebusy_jobs==1&&profile.hardwarebusy_cycles==77&&m.npu_jobs==1&&m.tensor_status.raw_jobs==1);
 assert(profile.quantize_ticks&&profile.rawexecute_ticks&&profile.scale_reduce_ticks);
 scales[0]=0;assert(matrix(&m,&odd,input,output,0)==QW_DEVICE);
 assert(m.npu_jobs==2&&m.tensor_status.raw_jobs==2&&profile.jobs_completed==2);
 assert(m.tensor_status.numeric.invalid_inputs==1);
 scales[0]=0x3c00;m.tensor_status.numeric.status&=~NPU_FIXED_STATUS_ERROR_MASK;
 /* Caller-selected small row tiles still need aligned device scale bases. */
 m.options.tile_rows=1;odd.n=3;int32_t tiled[3];
 assert(matrix(&m,&odd,input,tiled,0)==0);
 assert(!tiled[0]&&!tiled[1]&&!tiled[2]);
 m.options.tile_rows=32;

 /* Full causal softmax crosses the 32-token primitive tile boundary. */
 q=(qw_tensor){.n=4096,.k=1024,.dtype=QT_W4,.stride=512,.scales=scales};
 k=v=(qw_tensor){.n=512,.k=1024,.dtype=QT_W4,.stride=512,.scales=scales};
 out=(qw_tensor){.n=1024,.k=2048,.dtype=QT_W4,.stride=1024,.scales=scales};
 qn=(qw_tensor){.count=256,.dtype=QT_BF16,.data=norm_data};
 m.layers[3].q=&q;m.layers[3].k=&k;m.layers[3].v=&v;m.layers[3].out=&out;m.layers[3].qn=m.layers[3].kn=&qn;
 m.position=35;int32_t *values=(int32_t*)m.kv+CONTEXT*512;
 for(unsigned p=0;p<35;p++)for(unsigned i=0;i<512;i++)values[p*512+i]=65536;
 assert(attention(&m,3)==0);
 assert(mat32==8&&mat4==8&&vec32==8&&vec4==8&&soft36==8);
 /* Reference softmax is RNE(65536/36)=1820, gate(0)=32768. */
 assert(work(&m)->attention[0]==31850);

 /* UNKNOWN preserves both model/scratch ownership across all lifecycle APIs. */
 npu_tensor_job copy={.opcode=NPU_TENSOR_COPY,.n=1,.a=address(&m,work(&m)->a),.out=address(&m,work(&m)->out)};
 unknown_next=1;assert(submit(&m,&copy,0)==QW_UNKNOWN&&m.tensor_status.unknown);
 assert(qw_reset(&m)==QW_UNKNOWN&&qw_close(&m)==QW_UNKNOWN&&m.magic==QW_MAGIC);
 assert(qw_tensor_disable(&m)==QW_UNKNOWN&&qw_set_tile_rows(&m,1)==QW_UNKNOWN&&qw_set_lut_nonlinear(&m,1)==QW_UNKNOWN);
 qw_status status;assert(qw_get_status(&m,&status)==0&&status.lease_active);
 m.raw.cache_unknown=1;assert(qw_tensor_acknowledge_recovery(&m)==QW_UNKNOWN);
 m.raw.cache_unknown=0;assert(qw_tensor_acknowledge_recovery(&m)==0&&m.needs_reset);
 m.raw.cache_unknown=1;assert(qw_reset(&m)==QW_UNKNOWN&&qw_close(&m)==QW_UNKNOWN&&m.magic==QW_MAGIC);
 free(m.w);free(memory);puts("PASS_QW_TENSOR_ADAPTER padding scale profile context36 unknown");return 0;
}
