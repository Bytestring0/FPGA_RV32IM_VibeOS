#ifndef AG_NPU_STORAGE_H
#define AG_NPU_STORAGE_H

#include "npu_package.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NPU_STORAGE_BLOCK_BYTES 512u

/* Agent-owned file/Flash/TF access. Return 0 ONLY after writing exactly bytes
 * bytes to dst; any error, EOF or short read must return nonzero. byte_offset
 * is relative to the source (not necessarily the physical device). No seek
 * state is assumed. The callback must accept byte-unaligned offsets/dst. */
typedef int (*npu_storage_read_exact_at)(void *ctx, uint64_t byte_offset,
                                         void *dst, size_t bytes);

typedef struct {
    npu_storage_read_exact_at read_exact_at;
    void *ctx;
    uint64_t size_bytes; /* immutable size snapshot for this load */
} npu_storage_source;

/* Caller-owned state; initialize before use and do not modify during reads.
 * source is copied; its ctx and backing bytes must remain valid/stable.
 * max_read_span is the allowed source WINDOW length, not an I/O chunk limit. */
typedef struct {
    npu_storage_source source;
    uint64_t package_byte_offset;
    uint64_t max_read_span;
    int last_error;
} npu_storage_adapter;

int npu_storage_adapter_init(npu_storage_adapter *adapter,
                             const npu_storage_source *source,
                             uint64_t package_byte_offset,
                             uint64_t max_read_span);

/* Matches npu_package_read_blocks. lba is PACKAGE-RELATIVE, in 512-byte
 * blocks. Use start_lba=0 with npu_package_load. Range errors are E_RANGE,
 * callback failures (including short reads) E_IO. First failure is sticky. */
int npu_storage_read_blocks(void *ctx, uint32_t lba, uint32_t count,
                             void *destination);

/* Loads and validates via the existing npu_package_load; no allocation.
 * capacity is the actual caller-reserved DDR package budget, never an
 * assumed 1 GiB. All offsets/lengths are checked before narrowing to the
 * existing loader's uint32_t capacity. view is zero on failure, buffer may
 * contain partial/untrusted data. Only a successful view may be consumed. */
int npu_package_load_from_source(const npu_storage_source *source,
                                  uint64_t package_byte_offset,
                                  uint64_t max_read_span,
                                  void *buffer, uint64_t capacity,
                                  npu_package_view *view);

#ifdef __cplusplus
}
#endif
#endif
