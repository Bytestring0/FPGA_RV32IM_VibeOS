#ifndef NPU_FUSED_MATRIX_H
#define NPU_FUSED_MATRIX_H
#include "npu_raw.h"
#include "npu_fixed_math.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NPU_FUSED_MODE 0x4cu
#define NPU_FUSED_SCALE 0x50u
#define NPU_FUSED_MAXIMUM 0x54u
#define NPU_FUSED_FLAGS 0x58u
#define NPU_FUSED_SATURATIONS 0x5cu
#define NPU_FUSED_UNDERFLOWS 0x60u
#define NPU_FUSED_INVALID 0x64u
typedef struct {
 npu_raw_job raw;
 uint32_t scale_offset,output_offset,maximum;
 /* Caller-owned enclosing DDR regions; scale is immutable, output exclusive. */
 uint32_t scale_region_offset,scale_region_bytes;
 uint32_t output_region_offset,output_region_bytes;
} npu_fused_matrix_job;
typedef struct {uint32_t cycles,result_count,error;npu_fixed_status numeric;} npu_fused_matrix_stats;
/* Single caller/device owner, integer mode1 = exact tensor opcode19. The raw
 * context protects lifetime; explicit four-span cache publication precedes START,
 * output acquisition follows !BUSY + terminal response (all write ACKs).
 * UNKNOWN retains all four leases and leaves stats untouched. Never use raw
 * recover to discard this lease; use the explicit fused recovery below. */
int npu_fused_matrix_execute(npu_raw_context *,const npu_fused_matrix_job *,npu_raw_budgets,npu_fused_matrix_stats *);
/* Recovery proves quiescence/cache completion, discards results, then releases.
 * It is never invoked automatically or interpreted as successful inference. */
int npu_fused_matrix_recover(npu_raw_context *,uint32_t cache_polls);
#ifdef __cplusplus
}
#endif
#endif
