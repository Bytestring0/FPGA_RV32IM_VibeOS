#ifndef AG_NPU_RAW_H
#define AG_NPU_RAW_H

#include <stdint.h>
#include <stddef.h>
#include "npu_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NPU_RAW_API_VERSION 1u
#define NPU_RAW_DDR_LIMIT UINT32_C(0x40000000)
#define NPU_RAW_WEIGHT 1u
#define NPU_RAW_ACTIVATION 2u
#define NPU_RAW_PARTIAL 4u
#define NPU_RAW_ALL 7u

typedef enum {
    NPU_RAW_OK = 0,
    NPU_RAW_ARGUMENT = -1,
    NPU_RAW_REGION = -2,
    NPU_RAW_BUSY = -3,
    NPU_RAW_DEVICE = -4,
    NPU_RAW_UNKNOWN = -5,
    NPU_RAW_HOST_UNSUPPORTED = -6,
    NPU_RAW_TAG = -7
} npu_raw_status;

typedef enum {
    NPU_RAW_SOFTWARE_REFERENCE = 0,
    NPU_RAW_HARDWARE = 1
} npu_raw_mode;

/* DDR byte offsets, never CPU pointers. global_row_base is software metadata. */
typedef struct {
    uint32_t rows, cols;
    uint32_t weight_offset, activation_offset, partial_offset;
    uint32_t weight_stride, activation_stride, output_stride;
    uint32_t global_row_base;
} npu_raw_job;

/* A complete span must fit inside one region with the corresponding permission.
 * Regions are caller-owned; their table must remain stable for the context life. */
typedef struct {
    uint32_t offset, size, permissions;
} npu_raw_region;

typedef struct {
    uint32_t groups, activation_lines;
    uint32_t weight_stride, activation_stride, output_stride;
    uint32_t weight_span, activation_span, partial_span;
    uint32_t activation_read_span, partial_record_span;
    uint32_t record_bytes, row_tag_offset, group_tag_offset, partial_value_offset;
    uint32_t buffer_alignment, stride_alignment, record_alignment;
    uint32_t expected_result_count;
} npu_raw_layout;

/* base maps DDR [ddr_offset, ddr_offset+capacity). No fixed host/CPU mapping. */
typedef struct {
    unsigned char *base;
    size_t capacity;
    uint32_t ddr_offset;
} npu_raw_mapping;

typedef struct {
    uint32_t device_polls, cache_polls;
} npu_raw_budgets;

/* Borrowed from context: valid is reset by an accepted execute or successful
 * recovery. Never retain a struct copy as evidence that a buffer is still valid.
 * cycles is a hardware CSR snapshot; software reference returns zero. */
typedef struct {
    uint32_t physical_ddr_offset, stride, rows, groups, global_row_base;
    uint32_t valid, cycles, result_count, device_id;
} npu_raw_result_view;

/* Return zero only after maintenance completed. A nonzero return means UNKNOWN,
 * not cancellation. recover together with the device BUSY/ACK contract must
 * prove leased device/cache-maintenance traffic quiescent; it must not publish old partial output as a successful result.
 * Callbacks must be integer-only on targets without an F extension. */
typedef int (*npu_raw_cache_fn)(void *user, const npu_raw_job *job,
                              const npu_raw_layout *layout, uint32_t cache_polls);
typedef int (*npu_raw_recover_fn)(void *user, uint32_t cache_polls);

typedef struct {
    void *user;
    npu_raw_cache_fn publish, acquire;
    npu_raw_recover_fn recover;
} npu_raw_platform;

typedef struct {
    npu_raw_mode mode;
    volatile npu_dma_regs *device;
    npu_raw_platform platform;
    const npu_raw_region *regions;
    size_t region_count;
    npu_raw_mapping mapping; /* Required only by software reference. */
} npu_raw_config;

struct npu_raw_context;
typedef struct {
    int (*execute)(struct npu_raw_context *, const npu_raw_job *, npu_raw_budgets);
    int (*recover)(struct npu_raw_context *, uint32_t cache_polls);
} npu_raw_ops;

/* One device, one caller; not thread/interrupt safe. active=1 is a lease on all
 * spans in lease_job/lease_layout, including UNKNOWN. Do not reinitialize an
 * active context, reuse leased buffers, or access device through another owner. */
typedef struct npu_raw_context {
    const npu_raw_ops *ops;
    npu_raw_config config;
    npu_raw_job lease_job;
    npu_raw_layout lease_layout;
    npu_raw_result_view result;
    uint32_t active, cache_unknown, device_error;
    int last_status;
} npu_raw_context;

int npu_raw_query(const npu_raw_job *job, const npu_raw_region *regions,
                  size_t region_count, npu_raw_layout *layout);
/* init only on a new/quiescent object, not as a way to discard UNKNOWN. */
int npu_raw_init(npu_raw_context *context, const npu_raw_config *config);
int npu_raw_execute(npu_raw_context *context, const npu_raw_job *job,
                    npu_raw_budgets budgets, const npu_raw_result_view **view);
int npu_raw_recover(npu_raw_context *context, uint32_t cache_polls);

/* PC oracle only. Validates job, exact queried layout, regions and mapping
 * before any store; no MMIO, quantization, scaling, allocation or target claim.
 * Do not call on buffers held by an active hardware context. */
int npu_raw_reference(const npu_raw_job *job, const npu_raw_layout *layout,
                      const npu_raw_region *regions, size_t region_count,
                      const npu_raw_mapping *mapping);
int npu_raw_validate_tags(const npu_raw_result_view *view,
                          const npu_raw_mapping *mapping);

#ifdef __cplusplus
}
#endif
#endif
