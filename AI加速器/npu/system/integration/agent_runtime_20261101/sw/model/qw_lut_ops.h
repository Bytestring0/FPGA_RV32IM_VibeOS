#ifndef QW_LUT_OPS_H
#define QW_LUT_OPS_H
#include "qw_runtime.h"
#include "npu_fixed_math.h"
#ifdef __cplusplus
extern "C" {
#endif

#define QW_LUT_NUMERIC_VERSION NPU_FIXED_Q16_LUT_NUMERIC_VERSION
/* Snapshot between runtime calls. Counters saturate, persist across reset and
 * policy switches, and belong to one model (never global). DOMAIN_CLAMP reports
 * LUT tails; SATURATED/NONFINITE/INVALID_ARGUMENT abort the current token. */
typedef struct {
 uint32_t enabled,numeric_version;
 npu_fixed_status fixed;
 uint64_t sigmoid_calls,silu_calls,softplus_calls,exp_negative_calls;
} qw_lut_status;
/* At position zero only: reset before changing policy after any token. Neither
 * switching nor reset clears lifetime status. Active/unknown leases rejected. */
int qw_set_lut_nonlinear(qw_model *,uint32_t enabled);
int qw_get_lut_status(const qw_model *,qw_lut_status *);

/* IEEE binary32 STORAGE only. Sigmoid/SiLU/softplus decode RNE directly to
 * Q16, use LINEAR Q16 LUTs, and pack Q16 to binary32 with integer RNE. The
 * unsaturated input conversion error is at most 0.5/65536. Exp retains the
 * Q16 then RNE shift5 to Q11 path and unsigned Q30 output (input error at
 * most 16.5/65536). No Q12 intermediate is used by these runtime adapters.
 * Flags accumulate in optional status; bit interfaces return storage even on
 * failure, so callers must inspect ERROR_MASK|SATURATED before using it. */
uint32_t qw_lut_sigmoid_bits(uint32_t,npu_fixed_status *);
uint32_t qw_lut_silu_bits(uint32_t,npu_fixed_status *);
uint32_t qw_lut_softplus_bits(uint32_t,npu_fixed_status *);
/* The actual max-subtracted score, not the unshifted logit. Finite values
 * outside [-16,0] clamp with DOMAIN_CLAMP; exp(-16) remains nonzero. */
uint32_t qw_lut_exp_negative_bits(uint32_t,npu_fixed_status *);
/* Float ABI adapters perform memcpy only on float arguments/output storage;
 * all decoding, LUT arithmetic, packing and status work uses integers. Output
 * is untouched on QW_NUMERIC/QW_ARGUMENT. No FP casts, operators or mathlib. */
int qw_lut_sigmoid(float,float *,qw_lut_status *);
int qw_lut_silu(float,float *,qw_lut_status *);
int qw_lut_softplus(float,float *,qw_lut_status *);
int qw_lut_exp_negative(float,float *,qw_lut_status *);
#ifdef __cplusplus
}
#endif
#endif
