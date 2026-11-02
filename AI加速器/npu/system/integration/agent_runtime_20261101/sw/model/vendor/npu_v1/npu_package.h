#ifndef AG_NPU_PACKAGE_H
#define AG_NPU_PACKAGE_H
#include <stdint.h>
#include <stddef.h>

#define NPU_PACKAGE_VERSION 1u
#define NPU_PACKAGE_HEADER_BYTES 512u
#define NPU_PACKAGE_TENSOR_BYTES 64u
#define NPU_PACKAGE_ALIGN 512u
#define NPU_PACKAGE_MAX_DDR_BYTES UINT32_C(0x40000000)
#define NPU_PACKAGE_MAGIC "AGNPUPK\0"
#define NPU_PACKAGE_GGUF_MAGIC "GGUF"

enum {
    NPU_PACKAGE_OK = 0,
    NPU_PACKAGE_E_ARG = -1,
    NPU_PACKAGE_E_IO = -2,
    NPU_PACKAGE_E_MAGIC = -3,
    NPU_PACKAGE_E_VERSION = -4,
    NPU_PACKAGE_E_FORMAT = -5,
    NPU_PACKAGE_E_RANGE = -6,
    NPU_PACKAGE_E_CRC = -7,
    NPU_PACKAGE_E_NOT_FOUND = -8,
    NPU_PACKAGE_E_CAPACITY = -9,
    NPU_PACKAGE_E_GGUF = -10
};

typedef int (*npu_package_read_blocks)(void *ctx, uint32_t lba,
                                       uint32_t count, void *destination);

typedef struct {
    uint32_t name_id, flags, n, k, padded_k, row_stride, shared_const_id;
    uint64_t weight_offset, weight_length, scale_offset, scale_length;
} npu_package_tensor;

typedef struct {
    uint8_t *buffer;
    uint32_t bytes, tensor_count;
    uint32_t tensor_table_offset, tensor_record_bytes;
    uint32_t payload_offset;
} npu_package_view;

/* Reads a whole sector-aligned package into buffer and validates all structure,
 * payload bounds, and the full-package CRC32. The callback may be sd_card_read_blocks
 * through a small adapter matching this signature. No memory is allocated. */
int npu_package_load(npu_package_read_blocks read_blocks, void *ctx,
                     uint32_t start_lba, void *buffer, uint32_t capacity,
                     npu_package_view *view);
int npu_package_tensor_at(const npu_package_view *view, uint32_t index,
                          npu_package_tensor *tensor);
int npu_package_find(const npu_package_view *view, uint32_t name_id,
                     npu_package_tensor *tensor);
int npu_package_ddr_offset(const npu_package_view *view, uint32_t package_ddr_base,
                           uint64_t offset, uint64_t length, uint32_t *result);
uint32_t npu_package_name_id(const char *name);
uint32_t npu_package_crc32(const void *data, size_t bytes);

#endif
