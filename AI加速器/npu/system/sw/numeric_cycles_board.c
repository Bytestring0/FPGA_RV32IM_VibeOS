#include "platform.h"
#include "npu_fixed_math.h"
int32_t old_bf16(uint16_t,npu_fixed_status*);
int32_t old_f32(uint32_t,npu_fixed_status*);
static volatile uint32_t sink;
static void metric(const char*name,uint64_t value){puts_uart(name);puthex((uint32_t)(value>>32));puthex((uint32_t)value);puts_uart("\n");}
int main(void){
 npu_fixed_status old={0},fast={0};uint32_t old_sum=0,fast_sum=0;uint64_t start;
 start=read_cycles();for(unsigned i=0;i<4096;i++)old_sum+=(uint32_t)old_bf16((uint16_t)(0x3800u+(i*7u)%2048u),&old);metric("OLD_BF16_CYCLES=",read_cycles()-start);
 start=read_cycles();for(unsigned i=0;i<4096;i++)fast_sum+=(uint32_t)npu_fixed_bf16_bits_to_q16((uint16_t)(0x3800u+(i*7u)%2048u),&fast);metric("FAST_BF16_CYCLES=",read_cycles()-start);
 start=read_cycles();for(unsigned i=0;i<4096;i++)old_sum+=(uint32_t)old_f32(0x38000000u+(i*112177u)%0x8000000u,&old);metric("OLD_F32_CYCLES=",read_cycles()-start);
 start=read_cycles();for(unsigned i=0;i<4096;i++)fast_sum+=(uint32_t)npu_fixed_f32_bits_to_q16(0x38000000u+(i*112177u)%0x8000000u,&fast);metric("FAST_F32_CYCLES=",read_cycles()-start);
 sink=fast_sum;if(old_sum!=fast_sum||old.status!=fast.status||old.saturations!=fast.saturations||old.underflows!=fast.underflows)return 1;
 puts_uart("NUMERIC_RV32_CYCLES_PASS arithmetic_and_flags_equal scope=TCM_functions_no_PHY\n");return 0;
}
