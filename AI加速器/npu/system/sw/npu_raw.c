#include "npu_raw.h"

static int region_table_valid(const npu_raw_region *regions, size_t count) {
    size_t i;
    if (!regions || !count) return 0;
    for (i = 0; i < count; ++i) {
        if (!regions[i].size || !regions[i].permissions ||
            (regions[i].permissions & ~NPU_RAW_ALL) ||
            (uint64_t)regions[i].offset + regions[i].size > NPU_RAW_DDR_LIMIT)
            return 0;
    }
    return 1;
}

static int owned(const npu_raw_region *regions, size_t count,
                 uint32_t offset, uint32_t span, uint32_t permission) {
    size_t i;
    uint64_t end = (uint64_t)offset + span;
    for (i = 0; i < count; ++i) {
        if ((regions[i].permissions & permission) == permission &&
            offset >= regions[i].offset &&
            end <= (uint64_t)regions[i].offset + regions[i].size) return 1;
    }
    return 0;
}

static int overlap(uint32_t a, uint64_t ae, uint32_t b, uint64_t be) {
    return a < be && b < ae;
}

int npu_raw_query(const npu_raw_job *j, const npu_raw_region *regions,
                  size_t region_count, npu_raw_layout *out) {
    npu_raw_layout l;
    uint64_t wspan, aspan, ospan, wend, aend, oend;
    uint32_t row_bytes, bus_row_bytes;
    if (!j || !out || !j->rows || j->rows > 65535u ||
        !j->cols || j->cols > 4096u ||
        (uint64_t)j->global_row_base + j->rows - 1u > UINT32_MAX)
        return NPU_RAW_ARGUMENT;
    if (!region_table_valid(regions, region_count)) return NPU_RAW_REGION;
    l.groups = (j->cols + 127u) / 128u;
    l.activation_lines = (j->cols + 31u) / 32u;
    row_bytes = l.groups * 8u;
    bus_row_bytes = (row_bytes + 31u) & ~31u;
    l.weight_stride = j->weight_stride ? j->weight_stride : l.groups * 64u;
    l.activation_stride = j->activation_stride ? j->activation_stride : 32u;
    l.output_stride = j->output_stride ? j->output_stride : bus_row_bytes;
    if ((j->weight_offset | j->activation_offset | j->partial_offset |
         l.weight_stride | l.activation_stride | l.output_stride) & 31u)
        return NPU_RAW_ARGUMENT;
    if (l.weight_stride < l.groups * 64u || l.activation_stride < 32u ||
        l.output_stride < row_bytes) return NPU_RAW_ARGUMENT;
    wspan = (uint64_t)(j->rows - 1u) * l.weight_stride + l.groups * 64u;
    /* RTL's stride/range/overlap validator includes the last stride in full. */
    aspan = (uint64_t)l.activation_lines * l.activation_stride;
    /* Even a final 8-byte strobed record occupies a 32-byte bus request. */
    ospan = (uint64_t)(j->rows - 1u) * l.output_stride + bus_row_bytes;
    wend = (uint64_t)j->weight_offset + wspan;
    aend = (uint64_t)j->activation_offset + aspan;
    oend = (uint64_t)j->partial_offset + ospan;
    if (wend > NPU_RAW_DDR_LIMIT || aend > NPU_RAW_DDR_LIMIT ||
        oend > NPU_RAW_DDR_LIMIT) return NPU_RAW_ARGUMENT;
    if (overlap(j->weight_offset, wend, j->activation_offset, aend) ||
        overlap(j->weight_offset, wend, j->partial_offset, oend) ||
        overlap(j->activation_offset, aend, j->partial_offset, oend))
        return NPU_RAW_ARGUMENT;
    l.weight_span = (uint32_t)wspan;
    l.activation_span = (uint32_t)aspan;
    l.partial_span = (uint32_t)ospan;
    l.activation_read_span = (uint32_t)(aspan - l.activation_stride + 32u);
    l.partial_record_span = (uint32_t)(ospan - bus_row_bytes + row_bytes);
    l.record_bytes = 8u;
    l.row_tag_offset = 0u;
    l.group_tag_offset = 2u;
    l.partial_value_offset = 4u;
    l.buffer_alignment = 32u;
    l.stride_alignment = 32u;
    l.record_alignment = 8u;
    l.expected_result_count = j->rows * l.groups;
    if (!owned(regions, region_count, j->weight_offset, l.weight_span, NPU_RAW_WEIGHT) ||
        !owned(regions, region_count, j->activation_offset, l.activation_span, NPU_RAW_ACTIVATION) ||
        !owned(regions, region_count, j->partial_offset, l.partial_span, NPU_RAW_PARTIAL))
        return NPU_RAW_REGION;
    *out = l;
    return NPU_RAW_OK;
}

static void begin_lease(npu_raw_context *c, const npu_raw_job *j,
                        const npu_raw_layout *l) {
    c->lease_job = *j;
    c->lease_layout = *l;
    c->result.valid = 0u;
    c->active = 1u;
    c->cache_unknown = 0u;
    c->device_error = 0u;
}

static void finish_view(npu_raw_context *c, uint32_t cycles,
                        uint32_t count, uint32_t id) {
    c->result.physical_ddr_offset = c->lease_job.partial_offset;
    c->result.stride = c->lease_layout.output_stride;
    c->result.rows = c->lease_job.rows;
    c->result.groups = c->lease_layout.groups;
    c->result.global_row_base = c->lease_job.global_row_base;
    c->result.cycles = cycles;
    c->result.result_count = count;
    c->result.device_id = id;
    c->result.valid = 1u;
    c->active = 0u;
}

static int reference_execute(npu_raw_context *c, const npu_raw_job *j,
                              npu_raw_budgets budgets) {
    npu_raw_layout l;
    int rc;
    (void)budgets;
    rc = npu_raw_query(j, c->config.regions, c->config.region_count, &l);
    if (rc) return rc;
    begin_lease(c, j, &l);
    rc = npu_raw_reference(&c->lease_job, &c->lease_layout, c->config.regions,
                           c->config.region_count, &c->config.mapping);
    c->active = 0u; /* Reference is synchronous and has no external requests. */
    if (!rc) finish_view(c, 0u, l.expected_result_count, 0u);
    return rc;
}

static int reference_recover(npu_raw_context *c, uint32_t cache_polls) {
    (void)cache_polls;
    c->result.valid = 0u;
    c->active = 0u;
    c->cache_unknown = 0u;
    return NPU_RAW_OK;
}

static int hardware_execute(npu_raw_context *c, const npu_raw_job *j,
                             npu_raw_budgets budgets) {
    npu_raw_layout l;
    int rc = npu_raw_query(j, c->config.regions, c->config.region_count, &l);
    if (rc) return rc;
    if (!budgets.device_polls || !budgets.cache_polls) return NPU_RAW_ARGUMENT;
#if !defined(__riscv)
    return NPU_RAW_HOST_UNSUPPORTED;
#else
    volatile npu_dma_regs *r = c->config.device;
    uint32_t status = 0u, polls = budgets.device_polls;
    if (r->id != NPU_DMA_ID_VALUE) { c->result.valid=0u; return NPU_RAW_DEVICE; }
    if (r->status & NPU_DMA_BUSY) return NPU_RAW_BUSY;
    begin_lease(c, j, &l);
    if (c->config.platform.publish(c->config.platform.user, &c->lease_job,
                                  &c->lease_layout, budgets.cache_polls)) {
        c->cache_unknown = 1u;
        return NPU_RAW_UNKNOWN;
    }
    /* Platform callbacks supply cache maintenance + compiler/CPU ordering. */
    npu_dma_clear(r);
    npu_dma_program(r, j->rows, j->cols, j->weight_offset, j->activation_offset,
                    j->partial_offset, l.weight_stride, l.activation_stride,
                    l.output_stride);
    npu_dma_start(r);
    while (polls--) {
        status = r->status;
        if (!(status & NPU_DMA_BUSY) &&
            (status & (NPU_DMA_DONE | NPU_DMA_ERROR))) break;
    }
    if ((status & NPU_DMA_BUSY) || !(status & (NPU_DMA_DONE | NPU_DMA_ERROR)))
        return NPU_RAW_UNKNOWN;
    c->device_error = r->error_code;
    if (c->config.platform.acquire(c->config.platform.user, &c->lease_job,
                                  &c->lease_layout, budgets.cache_polls)) {
        c->cache_unknown = 1u;
        return NPU_RAW_UNKNOWN;
    }
    c->active = 0u;
    if (status & NPU_DMA_ERROR) return NPU_RAW_DEVICE;
    if (r->result_count != l.expected_result_count) return NPU_RAW_DEVICE;
    finish_view(c, r->cycle_count, r->result_count, r->id);
    return NPU_RAW_OK;
#endif
}

static int hardware_recover(npu_raw_context *c, uint32_t cache_polls) {
    if (!cache_polls) return NPU_RAW_ARGUMENT;
#if !defined(__riscv)
    (void)c;
    return NPU_RAW_HOST_UNSUPPORTED;
#else
    volatile npu_dma_regs *r = c->config.device;
    /* Recovery itself may start asynchronous cache maintenance. Even recovery
     * requested from idle must retain ownership if quiescence is uncertain. */
    c->active = 1u;
    c->result.valid = 0u;
    if (r->status & NPU_DMA_BUSY) return NPU_RAW_UNKNOWN;
    if (c->config.platform.recover(c->config.platform.user, cache_polls)) {
        c->cache_unknown = 1u;
        return NPU_RAW_UNKNOWN;
    }
    if (r->status & NPU_DMA_BUSY) return NPU_RAW_UNKNOWN;
    npu_dma_clear(r);
    c->result.valid = 0u;
    c->active = 0u;
    c->cache_unknown = 0u;
    c->device_error = 0u;
    return NPU_RAW_OK;
#endif
}

static const npu_raw_ops reference_ops = { reference_execute, reference_recover };
static const npu_raw_ops hardware_ops = { hardware_execute, hardware_recover };

int npu_raw_init(npu_raw_context *c, const npu_raw_config *config) {
    npu_raw_context empty = {0};
    if (!c || !config) return NPU_RAW_ARGUMENT;
    if (!region_table_valid(config->regions, config->region_count)) return NPU_RAW_REGION;
    if (config->mode == NPU_RAW_HARDWARE) {
        if (!config->device || !config->platform.publish ||
            !config->platform.acquire || !config->platform.recover)
            return NPU_RAW_ARGUMENT;
    } else if (config->mode == NPU_RAW_SOFTWARE_REFERENCE) {
        if (!config->mapping.base || !config->mapping.capacity ||
            config->mapping.ddr_offset >= NPU_RAW_DDR_LIMIT ||
            config->mapping.capacity > (size_t)(NPU_RAW_DDR_LIMIT - config->mapping.ddr_offset))
            return NPU_RAW_ARGUMENT;
    } else return NPU_RAW_ARGUMENT;
    *c = empty;
    c->config = *config;
    c->ops = config->mode == NPU_RAW_HARDWARE ? &hardware_ops : &reference_ops;
    return NPU_RAW_OK;
}

int npu_raw_execute(npu_raw_context *c, const npu_raw_job *j,
                    npu_raw_budgets budgets, const npu_raw_result_view **view) {
    int rc;
    if (view) *view = NULL;
    if (!c || !c->ops || !j || !view) return NPU_RAW_ARGUMENT;
    if (c->active) return NPU_RAW_BUSY;
    rc = c->ops->execute(c, j, budgets);
    c->last_status = rc;
    if (!rc) *view = &c->result;
    return rc;
}

int npu_raw_recover(npu_raw_context *c, uint32_t cache_polls) {
    int rc;
    if (!c || !c->ops) return NPU_RAW_ARGUMENT;
    rc = c->ops->recover(c, cache_polls);
    c->last_status = rc;
    return rc;
}
