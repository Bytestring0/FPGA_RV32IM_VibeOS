#ifndef AG_NPU_DMA_H
#define AG_NPU_DMA_H
#include <stdint.h>
#include <stddef.h>

/* All offsets are DDR byte offsets (0 <= offset < 1 GiB), not CPU addresses. */
#define NPU_DMA_CSR_BASE UINT32_C(0x10004000)
#define NPU_DMA_CTRL             0x00u
#define NPU_DMA_STATUS           0x04u
#define NPU_DMA_N                0x08u
#define NPU_DMA_K                0x0cu
#define NPU_DMA_WEIGHT_LO        0x10u
#define NPU_DMA_WEIGHT_HI        0x14u
#define NPU_DMA_ACTIVATION_LO    0x18u
#define NPU_DMA_ACTIVATION_HI    0x1cu
#define NPU_DMA_OUTPUT_LO        0x20u
#define NPU_DMA_OUTPUT_HI        0x24u
#define NPU_DMA_WEIGHT_STRIDE    0x28u
#define NPU_DMA_ACTIVATION_STRIDE 0x2cu
#define NPU_DMA_OUTPUT_STRIDE    0x30u
#define NPU_DMA_ERROR_CODE       0x34u
#define NPU_DMA_CYCLE_COUNT      0x38u
#define NPU_DMA_RESULT_COUNT     0x3cu
#define NPU_DMA_ID               0x40u
#define NPU_DMA_GROUPS           0x44u
#define NPU_DMA_ID_VALUE         UINT32_C(0x4e444d41)
#define NPU_DMA_START 1u
#define NPU_DMA_CLEAR 2u
#define NPU_DMA_BUSY  1u
#define NPU_DMA_DONE  2u
#define NPU_DMA_ERROR 4u
#define NPU_DMA_ERR_N       1u
#define NPU_DMA_ERR_K       2u
#define NPU_DMA_ERR_ALIGN   3u
#define NPU_DMA_ERR_STRIDE  4u
#define NPU_DMA_ERR_RANGE   5u
#define NPU_DMA_ERR_OVERLAP 6u
#define NPU_DMA_ERR_ACT_READ 7u
#define NPU_DMA_ERR_WEIGHT_READ 8u
#define NPU_DMA_ERR_OUTPUT_WRITE 9u

typedef struct {
    volatile uint32_t ctrl, status, n, k;
    volatile uint32_t weight_lo, weight_hi, activation_lo, activation_hi;
    volatile uint32_t output_lo, output_hi;
    volatile uint32_t weight_stride, activation_stride, output_stride;
    volatile uint32_t error_code, cycle_count, result_count, id, groups;
} npu_dma_regs;
/* DDR record in native little-endian order: local row, group, signed partial. */
typedef struct { uint16_t row, group; int32_t partial; } npu_dma_result;
/* No printf, allocation, or system dependencies. Caller manages cache coherency. */
static inline volatile npu_dma_regs *npu_dma_mmio(void) {
    return (volatile npu_dma_regs *)(uintptr_t)NPU_DMA_CSR_BASE;
}
static inline void npu_dma_clear(volatile npu_dma_regs *r) { r->ctrl = NPU_DMA_CLEAR; }
static inline void npu_dma_start(volatile npu_dma_regs *r) { r->ctrl = NPU_DMA_START; }
static inline void npu_dma_program(volatile npu_dma_regs *r, uint32_t n, uint32_t k,
                                   uint64_t weight_off, uint64_t activation_off, uint64_t output_off,
                                   uint32_t weight_stride, uint32_t activation_stride, uint32_t output_stride) {
    r->n=n; r->k=k;
    r->weight_lo=(uint32_t)weight_off; r->weight_hi=(uint32_t)(weight_off>>32);
    r->activation_lo=(uint32_t)activation_off; r->activation_hi=(uint32_t)(activation_off>>32);
    r->output_lo=(uint32_t)output_off; r->output_hi=(uint32_t)(output_off>>32);
    r->weight_stride=weight_stride; r->activation_stride=activation_stride;
    r->output_stride=output_stride;
}
/* Returns 0: success, -1: hardware error, -2: timeout. Clear before next task. */
static inline int npu_dma_wait(volatile npu_dma_regs *r, uint32_t polls, uint32_t *error_out) {
    while (polls--) {
        uint32_t s = r->status;
        if (s & NPU_DMA_ERROR) { if (error_out) *error_out=r->error_code; return -1; }
        if (s & NPU_DMA_DONE) { if (error_out) *error_out=0; return 0; }
    }
    return -2;
}
#endif
