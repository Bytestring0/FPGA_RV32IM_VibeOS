#ifndef AG_QW_INT_SAMPLE_H
#define AG_QW_INT_SAMPLE_H
#include "qw_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Integer-only greedy selection from n native-endian IEEE binary32 storage
 * words, read with memcpy (unaligned storage is supported). No FP arithmetic,
 * conversions, or math-library calls. The first index wins every exact tie;
 * positive and negative zero compare equal.
 *
 * Returns QW_ARGUMENT for NULL pointers or n outside [1,QW_VOCAB], QW_NUMERIC
 * for any NaN/infinity, and QW_OK otherwise. *token changes only on QW_OK.
 * Input must contain n complete words; no model, seed, or device is needed. */
int qw_sample_greedy_bits(const void *binary32_logits,size_t n,uint32_t *token);

#ifdef __cplusplus
}
#endif
#endif
