#ifndef QW_CHECKPOINT_H
#define QW_CHECKPOINT_H
#include "qw_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QW_CHECKPOINT_FORMAT_VERSION 1u
#define QW_CHECKPOINT_ALIGNMENT 32u

/* Tensor-v2 state checkpoint only. No allocation, floating point, DMA, or IO. */
int qw_checkpoint_size(const qw_model *model, size_t *bytes);
int qw_checkpoint_save(const qw_model *model, void *buffer, size_t capacity);
int qw_checkpoint_restore(qw_model *model, const void *buffer, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
