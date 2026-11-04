#include "npu_raw.h"

static int mapped(const npu_raw_mapping *m, uint32_t offset, uint32_t span) {
    size_t start;
    if (!m || !m->base || !m->capacity ||
        m->ddr_offset >= NPU_RAW_DDR_LIMIT ||
        m->capacity > (size_t)(NPU_RAW_DDR_LIMIT - m->ddr_offset) ||
        offset < m->ddr_offset) return 0;
    start = (size_t)(offset - m->ddr_offset);
    return start <= m->capacity && span <= m->capacity - start;
}

static int same_layout(const npu_raw_layout *a, const npu_raw_layout *b) {
    return a->groups == b->groups && a->activation_lines == b->activation_lines &&
        a->weight_stride == b->weight_stride && a->activation_stride == b->activation_stride &&
        a->output_stride == b->output_stride && a->weight_span == b->weight_span &&
        a->activation_span == b->activation_span && a->partial_span == b->partial_span &&
        a->activation_read_span == b->activation_read_span &&
        a->partial_record_span == b->partial_record_span &&
        a->record_bytes == b->record_bytes && a->row_tag_offset == b->row_tag_offset &&
        a->group_tag_offset == b->group_tag_offset &&
        a->partial_value_offset == b->partial_value_offset &&
        a->buffer_alignment == b->buffer_alignment &&
        a->stride_alignment == b->stride_alignment &&
        a->record_alignment == b->record_alignment &&
        a->expected_result_count == b->expected_result_count;
}

static unsigned char *pointer(const npu_raw_mapping *m, uint32_t offset) {
    return m->base + (size_t)(offset - m->ddr_offset);
}

static void put16(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

int npu_raw_reference(const npu_raw_job *j, const npu_raw_layout *layout,
                      const npu_raw_region *regions, size_t region_count,
                      const npu_raw_mapping *m) {
    npu_raw_layout l;
    const unsigned char *weights, *activation;
    unsigned char *output;
    uint32_t row, group;
    int rc;
    if (!layout) return NPU_RAW_ARGUMENT;
    rc = npu_raw_query(j, regions, region_count, &l);
    if (rc) return rc;
    if (!same_layout(layout, &l)) return NPU_RAW_ARGUMENT;
    if (!mapped(m, j->weight_offset, l.weight_span) ||
        !mapped(m, j->activation_offset, l.activation_span) ||
        !mapped(m, j->partial_offset, l.partial_span)) return NPU_RAW_REGION;
    weights = pointer(m, j->weight_offset);
    activation = pointer(m, j->activation_offset);
    output = pointer(m, j->partial_offset);
    for (row = 0; row < j->rows; ++row) {
        for (group = 0; group < l.groups; ++group) {
            uint32_t col, begin = group * 128u, end = begin + 128u;
            int32_t sum = 0;
            unsigned char *record = output + (size_t)row * l.output_stride + group * 8u;
            if (end > j->cols) end = j->cols;
            for (col = begin; col < end; ++col) {
                uint32_t packed = weights[(size_t)row * l.weight_stride + col / 2u];
                uint32_t nibble = (packed >> ((col & 1u) * 4u)) & 15u;
                uint32_t byte = activation[(size_t)(col / 32u) * l.activation_stride + col % 32u];
                /* Explicit arithmetic avoids implementation-defined signed casts
                 * and signed right shifts. Last group ignores all padded lanes. */
                int32_t w = nibble >= 8u ? (int32_t)nibble - 16 : (int32_t)nibble;
                int32_t a = byte >= 128u ? (int32_t)byte - 256 : (int32_t)byte;
                sum += w * a;
            }
            put16(record, row);
            put16(record + 2u, group);
            put32(record + 4u, (uint32_t)sum);
        }
    }
    return NPU_RAW_OK;
}

int npu_raw_validate_tags(const npu_raw_result_view *v, const npu_raw_mapping *m) {
    uint64_t span;
    uint32_t row, group;
    const unsigned char *base;
    if (!v || !v->valid || !v->rows || v->rows > 65535u ||
        !v->groups || v->groups > 32u ||
        (v->physical_ddr_offset & 31u) || (v->stride & 31u) ||
        v->stride < v->groups * 8u ||
        (uint64_t)v->global_row_base + v->rows - 1u > UINT32_MAX)
        return NPU_RAW_ARGUMENT;
    span = (uint64_t)(v->rows - 1u) * v->stride + ((v->groups * 8u + 31u) & ~31u);
    if (span > NPU_RAW_DDR_LIMIT ||
        (uint64_t)v->physical_ddr_offset + span > NPU_RAW_DDR_LIMIT)
        return NPU_RAW_ARGUMENT;
    if (!mapped(m, v->physical_ddr_offset, (uint32_t)span)) return NPU_RAW_REGION;
    base = pointer(m, v->physical_ddr_offset);
    for (row = 0; row < v->rows; ++row) {
        for (group = 0; group < v->groups; ++group) {
            const unsigned char *p = base + (size_t)row * v->stride + group * 8u;
            uint32_t r = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            uint32_t g = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
            if (r != row || g != group) return NPU_RAW_TAG;
        }
    }
    return NPU_RAW_OK;
}
