#include "npu_storage.h"

static int window_fits(const npu_storage_source *source, uint64_t offset,
                        uint64_t span) {
    /* Subtraction checks reject both source overruns and offset+span overflow. */
    return offset <= source->size_bytes && span <= source->size_bytes - offset;
}

int npu_storage_adapter_init(npu_storage_adapter *adapter,
                             const npu_storage_source *source,
                             uint64_t package_byte_offset,
                             uint64_t max_read_span) {
    if (!adapter) return NPU_PACKAGE_E_ARG;
    *adapter = (npu_storage_adapter){0};
    if (!source || !source->read_exact_at) {
        adapter->last_error = NPU_PACKAGE_E_ARG;
        return adapter->last_error;
    }
    if (!window_fits(source, package_byte_offset, max_read_span) ||
        max_read_span < NPU_STORAGE_BLOCK_BYTES) {
        adapter->last_error = NPU_PACKAGE_E_RANGE;
        return adapter->last_error;
    }
    adapter->source = *source;
    adapter->package_byte_offset = package_byte_offset;
    adapter->max_read_span = max_read_span;
    return NPU_PACKAGE_OK;
}

int npu_storage_read_blocks(void *ctx, uint32_t lba, uint32_t count,
                             void *destination) {
    npu_storage_adapter *a = (npu_storage_adapter *)ctx;
    uint64_t relative, bytes, absolute;
    if (!a) return NPU_PACKAGE_E_ARG;
    if (a->last_error) return a->last_error;
    if (!a->source.read_exact_at || !destination || !count) {
        a->last_error = NPU_PACKAGE_E_ARG;
        return a->last_error;
    }
    /* Multiplications occur at 64 bits, including on RV32. */
    relative = (uint64_t)lba * NPU_STORAGE_BLOCK_BYTES;
    bytes = (uint64_t)count * NPU_STORAGE_BLOCK_BYTES;
    if ((uint64_t)lba + count > UINT64_C(0x100000000) ||
        !window_fits(&a->source, a->package_byte_offset, a->max_read_span) ||
        relative > a->max_read_span || bytes > a->max_read_span - relative ||
        bytes > SIZE_MAX || bytes - 1u > UINTPTR_MAX - (uintptr_t)destination) {
        a->last_error = NPU_PACKAGE_E_RANGE;
        return a->last_error;
    }
    /* The validated window proves both additions fit uint64_t and source. */
    absolute = a->package_byte_offset + relative;
    if (a->source.read_exact_at(a->source.ctx, absolute, destination,
                                (size_t)bytes) != 0) {
        a->last_error = NPU_PACKAGE_E_IO;
        return a->last_error;
    }
    return NPU_PACKAGE_OK;
}

int npu_package_load_from_source(const npu_storage_source *source,
                                  uint64_t package_byte_offset,
                                  uint64_t max_read_span,
                                  void *buffer, uint64_t capacity,
                                  npu_package_view *view) {
    npu_storage_adapter adapter;
    int rc;
    if (!view) return NPU_PACKAGE_E_ARG;
    *view = (npu_package_view){0};
    if (!buffer) return NPU_PACKAGE_E_ARG;
    if (capacity < NPU_STORAGE_BLOCK_BYTES ||
        capacity > NPU_PACKAGE_MAX_DDR_BYTES || capacity > UINT32_MAX ||
        capacity > SIZE_MAX) return NPU_PACKAGE_E_CAPACITY;
    if (capacity - 1u > UINTPTR_MAX - (uintptr_t)buffer)
        return NPU_PACKAGE_E_RANGE;
    rc = npu_storage_adapter_init(&adapter, source, package_byte_offset,
                                   max_read_span);
    if (rc) return rc;
    rc = npu_package_load(npu_storage_read_blocks, &adapter, 0u, buffer,
                          (uint32_t)capacity, view);
    /* The legacy loader collapses callback errors to E_IO; retain precise
     * adapter range/argument failures without changing the legacy loader. */
    if (adapter.last_error) return adapter.last_error;
    return rc;
}
