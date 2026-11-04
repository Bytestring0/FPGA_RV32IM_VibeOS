#include "npu_fused_matrix.h"
#include "npu_runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint32_t regs[32];static unsigned maintenance,started,acquired,timeout_mode,cache_fail,device_error,wrong_count;
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"failure line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
uint32_t npu_fused_test_read32(volatile uint32_t *p){unsigned at=(unsigned)(p-regs);if(at==1&&started){if(timeout_mode)return 1;return device_error?4:2;}return regs[at];}
void npu_fused_test_write32(volatile uint32_t *p,uint32_t value){unsigned at=(unsigned)(p-regs);regs[at]=value;if(at==0&&value==2){started=0;regs[1]=0;}if(at==0&&value==1){REQUIRE(maintenance==1);REQUIRE(regs[19]==1);REQUIRE(regs[20]==0x50002);REQUIRE(regs[21]==65536);REQUIRE(regs[12]==4);started=1;regs[15]=wrong_count?2:3;regs[14]=123;regs[13]=device_error?9:0;regs[22]=6;regs[23]=7;regs[24]=8;regs[25]=0;}}
int ag_cache_maintain_ranges(const ag_cache_range *spans,size_t count,uint32_t polls){REQUIRE(polls==20);maintenance++;if(count==4){REQUIRE(!started);REQUIRE(spans[0].offset==0x1000&&spans[0].bytes==384);REQUIRE(spans[1].offset==0x20000&&spans[1].bytes==160);REQUIRE(spans[2].offset==0x50002&&spans[2].bytes==12);REQUIRE(spans[3].offset==0x60000&&spans[3].bytes==12);}else {REQUIRE(count==1&&started&&!timeout_mode);REQUIRE(spans[0].offset==0x60000&&spans[0].bytes==12);acquired++;}return cache_fail?1:0;}
static void fresh(npu_raw_context *c){memset(c,0,sizeof *c);memset(regs,0,sizeof regs);regs[16]=NPU_DMA_ID_VALUE;c->config.mode=NPU_RAW_HARDWARE;c->config.device=(volatile npu_dma_regs*)regs;static npu_raw_region ranges[]={{0x1000,0x10000,NPU_RAW_WEIGHT},{0x20000,0x10000,NPU_RAW_ACTIVATION},{0x30000,0x10000,NPU_RAW_PARTIAL}};c->config.regions=ranges;c->config.region_count=3;maintenance=started=acquired=timeout_mode=cache_fail=device_error=wrong_count=0;}
int main(void){npu_raw_context c;npu_fused_matrix_stats s,prior;npu_fused_matrix_job j={0};j.raw.rows=3;j.raw.cols=129;j.raw.weight_offset=0x1000;j.raw.activation_offset=0x20000;j.raw.partial_offset=0x30000;j.scale_offset=0x50002;j.output_offset=0x60000;j.maximum=65536;j.scale_region_offset=0x50000;j.scale_region_bytes=0x10000;j.output_region_offset=0x60000;j.output_region_bytes=0x10000;npu_raw_budgets b={20,20};
 fresh(&c);REQUIRE(!npu_fused_matrix_execute(&c,&j,b,&s));REQUIRE(maintenance==2&&acquired==1&&!c.active&&!c.cache_unknown&&regs[19]==0);REQUIRE(s.result_count==3&&s.cycles==123&&s.numeric.status==6&&s.numeric.saturations==7&&s.numeric.underflows==8);
 fresh(&c);device_error=1;REQUIRE(npu_fused_matrix_execute(&c,&j,b,&s)==NPU_RAW_DEVICE);REQUIRE(acquired==1&&!c.active&&s.error==9&&regs[19]==0);
 fresh(&c);wrong_count=1;REQUIRE(npu_fused_matrix_execute(&c,&j,b,&s)==NPU_RAW_DEVICE);REQUIRE(acquired==1&&!c.active);
 fresh(&c);memset(&s,0xa5,sizeof s);prior=s;timeout_mode=1;REQUIRE(npu_fused_matrix_execute(&c,&j,b,&s)==NPU_RAW_UNKNOWN);REQUIRE(c.active&&c.cache_unknown&&!memcmp(&s,&prior,sizeof s)&&!acquired);REQUIRE(npu_fused_matrix_execute(&c,&j,b,&s)==NPU_RAW_UNKNOWN);REQUIRE(npu_fused_matrix_recover(&c,20)==NPU_RAW_UNKNOWN);timeout_mode=0;started=0;REQUIRE(!npu_fused_matrix_recover(&c,20));REQUIRE(!c.active&&!c.cache_unknown&&regs[19]==0);
 fresh(&c);cache_fail=1;REQUIRE(npu_fused_matrix_execute(&c,&j,b,&s)==NPU_RAW_UNKNOWN);REQUIRE(c.active&&c.cache_unknown&&!started&&!memcmp(&s,&prior,sizeof s));cache_fail=0;REQUIRE(!npu_fused_matrix_recover(&c,20));
 fresh(&c);j.scale_offset++;REQUIRE(npu_fused_matrix_execute(&c,&j,b,&s)==NPU_RAW_REGION);REQUIRE(!maintenance&&!started);j.scale_offset--;
 fresh(&c);j.output_offset=0x50000;j.output_region_offset=0x50000;REQUIRE(npu_fused_matrix_execute(&c,&j,b,&s)==NPU_RAW_ARGUMENT);REQUIRE(!maintenance&&!started);
 puts("FUSED_MATRIX_DRIVER_PASS publication4 acquire1 exact_mode1 partial_scale_line result_ack_count device_error timeout_lease recover cache_failure overlap_guard");return 0;}
