#ifndef AG_QW_INT_HEAD_H
#define AG_QW_INT_HEAD_H
#include "qw_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit experimental arithmetic version, distinct from the FP32 baseline.
 * Only the selected output head uses integer arithmetic. The rest of the graph
 * (including final normalization) still uses FP32. Model quality is NOT approved.
 * No nonlinear operation/LUT or output bias is added to the tied output head. */
#define QW_INTEGER_HEAD_NUMERIC_VERSION UINT32_C(0x00010000)

typedef struct {
 uint32_t enabled,numeric_version;
 /* Saturating lifetime counters; model reset preserves them and this mode.
  * numeric_errors counts QW_NUMERIC failures; saturations/underflows count events.
  * Underflow is declared quantization loss, not an execution error. */
 uint32_t numeric_errors,saturations,underflows;
} qw_integer_head_status;

/* Requires an already explicitly selected W4 head, a quiescent raw context,
 * and !needs_reset. Does not select/convert a tensor or recover UNKNOWN.
 * Returns QW_UNKNOWN for active/cache-unknown ownership, QW_NEED_RESET for a
 * pending reset, QW_ARGUMENT for an invalid handle, QW_FORMAT if no W4 head.
 * Success invalidates old logits when newly enabled. There is no implicit
 * enable or fallback; close/reopen restores the default FP32 arithmetic.
 * One caller only; do not invoke from a runtime callback. */
int qw_enable_integer_w4_head(qw_model *model);
/* Metadata only; does not establish device quiescence or numerical quality. */
int qw_get_integer_w4_head(const qw_model *model,qw_integer_head_status *status);

/* Internal forward entry point. input_bits contains QW_HIDDEN native IEEE
 * binary32 storage words; output_bits has room for QW_VOCAB binary32 words.
 * Both arguments are byte-accessed via memcpy, never dereferenced as float or
 * reinterpreted through an incompatible pointer. No FP arithmetic/helper calls.
 *
 * Finite input is decoded with integer RNE to shared-exponent int16: choose
 * the finest exponent for which max RNE magnitude <=32767 (exponent -163..114;
 * all-zero uses 0). Thus input[i] ~= int16[i]*2^exponent, with <=0.5 LSB error.
 * Quantize to [-127,127], RNE, with exact scale maxabs*2^exponent/127.
 * Each INT32 partial * FP16 scale bits * activation scale is RNE to int64 Q16;
 * sum in ascending group order and narrow to int32 Q16, with bias exactly zero.
 * The Q16 output range is [-32768,32768-1/65536], packed integer-RNE to binary32
 * for the existing greedy sampler (packing can collapse nearby Q16 values).
 * Saturation/overflow or nonfinite input/scale returns QW_NUMERIC and counts
 * the failure; partial output must not be published. Underflow is counted.
 * Raw query/execute uses the original context/grants and preserves UNKNOWN.
 * Caller handles the normal forward reset/error policy after a failed call. */
int qw_integer_head_execute(qw_model *model,const void *input_bits,void *output_bits);

#ifdef __cplusplus
}
#endif
#endif
