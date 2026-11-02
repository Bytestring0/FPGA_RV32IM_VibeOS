#ifndef AG_NPU_RUNTIME_H
#define AG_NPU_RUNTIME_H
#include <stdint.h>
#include <stddef.h>

/* freestanding services; a timeout is an error, never an abort acknowledgement */
int ag_cache_maintain(uint32_t poll_limit);
int ag_dma_publish(uint32_t poll_limit);
int ag_dma_acquire(uint32_t poll_limit);

#define AG_CACHE_MAX_RANGES 6u
typedef struct { uint32_t offset,bytes; } ag_cache_range;
/* DDR offsets, full32B lines are rounded outward. Up to6 spans are validated
 * before any MMIO, sorted/merged, then clean+invalidate I/D cache. Capability
 * absent => one legacy full-cache operation; original APIs above stay full.
 * Each merged range still scans every cache set. Poll bound is per range.
 * Single owner only. Timeout is UNKNOWN, never cancellation. */
int ag_cache_maintain_ranges(const ag_cache_range *ranges,size_t count,uint32_t poll_limit);
int ag_cache_maintain_range(uint32_t offset,uint32_t bytes,uint32_t poll_limit);


/* Exact on-wire record from both integer devices, little endian RV32. */
typedef struct { uint32_t tag; int32_t partial; } npu_partial;

/* FP16 scales are stored as IEEE binary16 bit patterns, one per row/group.
 * Output uses explicitly ordered FP32 scale/reduction operations. This helper
 * runs with the target's soft-float ABI; it is a correctness/reference path,
 * not a claim of hardware floating-point acceleration. */
int npu_reduce_partials(const npu_partial *records, size_t record_count,
                        uint32_t rows, uint32_t cols,
                        const uint16_t *weight_scales, float activation_scale,
                        const float *bias, float *output);
float npu_binary16_to_float(uint16_t bits);
int npu_reduce_strided(const void *records, size_t buffer_bytes,
                       uint32_t rows, uint32_t cols, uint32_t row_stride_bytes,
                       const uint16_t *weight_scales, float activation_scale,
                       const float *bias, float *output);

/* Candidate activation convention: per-vector symmetric INT8, range -127..127,
 * round-to-nearest ties-to-even, all-zero scale=1. Explicitly separate from
 * model calibration; no existing GGUF data is silently requantized. */
int npu_quantize_activation(const float *input, uint32_t count,
                            int8_t *output, float *scale);
#endif
