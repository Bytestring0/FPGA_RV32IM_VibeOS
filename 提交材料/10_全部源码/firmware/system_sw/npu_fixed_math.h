#ifndef NPU_FIXED_MATH_H
#define NPU_FIXED_MATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Candidate ABI/numerics version 1. Deliberately different from the existing
 * FP32 operation order. No runtime registration or default-model change.
 * Qn always means signed/unsigned integer storage divided by 2^n; the integer
 * width is stated separately. All rounding is nearest, ties to even (RNE).
 * No dynamic allocation, global mutable state, host endian assumptions, or
 * floating-point operations. Integer division helpers may be needed on RV32.
 */
#define NPU_FIXED_NUMERIC_VERSION UINT32_C(0x00010000)
/* Additive Q16 activation LUT profile; the original numeric ABI stays v1. */
#define NPU_FIXED_Q16_LUT_NUMERIC_VERSION UINT32_C(0x00010001)
#define NPU_FIXED_Q12_ONE INT32_C(4096)
#define NPU_FIXED_Q16_ONE INT32_C(65536)
#define NPU_FIXED_Q30_ONE UINT32_C(1073741824)
#define NPU_FIXED_Q15_ONE INT32_C(32768)

#define NPU_FIXED_STATUS_DOMAIN_CLAMP UINT32_C(1)
#define NPU_FIXED_STATUS_SATURATED UINT32_C(2)
#define NPU_FIXED_STATUS_UNDERFLOW UINT32_C(4)
#define NPU_FIXED_STATUS_INVALID_ARGUMENT UINT32_C(8)
#define NPU_FIXED_STATUS_NONFINITE UINT32_C(16)
#define NPU_FIXED_STATUS_ZERO_VECTOR UINT32_C(32)
#define NPU_FIXED_STATUS_ERROR_MASK \
    (NPU_FIXED_STATUS_INVALID_ARGUMENT | NPU_FIXED_STATUS_NONFINITE)

typedef enum {
    NPU_FIXED_LUT_NEAREST = 0, /* nearest knot, ties to even knot index */
    NPU_FIXED_LUT_LINEAR = 1   /* interpolate quantized knots, then RNE */
} npu_fixed_lut_mode;

typedef struct {
    uint32_t status;          /* sticky OR of the status flags above */
    uint32_t domain_clamps;   /* calls/outputs outside a LUT core domain */
    uint32_t saturations;     /* values clipped to a storage endpoint */
    uint32_t underflows;      /* nonzero bit/partial conversions rounded to zero */
    uint32_t invalid_inputs;  /* invalid argument or nonfinite events */
    uint32_t zero_vectors;    /* empty/all-zero quantized vectors */
} npu_fixed_status;
/* Counters count events (not real-value magnitudes) and saturate at UINT32_MAX.
 * Saturation counts each clipping stage: a huge scalar can clip at int64 and
 * again at int32, producing two events. LUT/quantizer zeros are not underflow
 * events. DOMAIN_CLAMP denotes tail extension as well as literal exp clamping.
 * Numeric output units are independent of counters. NULL status is allowed.
 * Status objects are caller-owned and are not safe for concurrent mutation. */
void npu_fixed_status_clear(npu_fixed_status *status);

/* Input: int32 Q12. Output: int32 Q12.
 * Core domain [-16,16], knot spacing 1/64. Outside that core, set DOMAIN_CLAMP
 * and use these tails: sigmoid {0,1}, tanh {-1,1}, SiLU {0,x}, softplus {0,x}.
 * Exactly +/-16 uses the endpoint knot and does not set DOMAIN_CLAMP.
 * Full int32 input range is accepted (including INT32_MIN). Sigmoid [0,4096],
 * tanh [-4096,4096], SiLU approximately [-1141,INT32_MAX], softplus >=0.
 * Invalid mode returns zero and sets INVALID_ARGUMENT.
 * LINEAR absolute error <=2 Q12 output units (=2/4096) for these four
 * functions on the representable inputs; NEAREST conservative bounds in Q12
 * units: sigmoid <=9, tanh <=33, SiLU <=37, softplus <=33. Bounds include
 * knot quantization, interpolation rounding, and the stated tails, but not
 * upstream input quantization. These are analytic bounds, not model quality.
 */
int32_t npu_fixed_sigmoid_q12(int32_t x, npu_fixed_lut_mode mode,
                            npu_fixed_status *status);
int32_t npu_fixed_tanh_q12(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *status);
int32_t npu_fixed_silu_q12(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *status);
int32_t npu_fixed_softplus_q12(int32_t x, npu_fixed_lut_mode mode,
                             npu_fixed_status *status);

/* Additive Q16 LUT profile 0x00010001: input/output are int32 Q16, with
 * direct Q16 lookup/interpolation and no Q12 intermediate. Core [-16,16],
 * knot spacing 1/64 (1024 input integer units), independent Q16 knot tables.
 * Domain/tail/mode/status semantics match the Q12 functions. Entire int32
 * input range is valid, including INT32_MIN. Sigmoid [0,65536], tanh
 * [-65536,65536], SiLU nonpositive for negative inputs and nonnegative for
 * positive inputs, softplus >=0. Exactly +/-16 uses endpoint knots with no
 * domain flag.
 * Conservative absolute error limits, in Q16 output units (1/65536):
 *              sigmoid   tanh   SiLU   softplus
 * LINEAR          2        3      3       2
 * NEAREST       129      513    564     513
 * Bounds include knot quantization, interpolation RNE, and asymptotic tails;
 * they exclude upstream input quantization. Invalid modes return zero and
 * flag INVALID_ARGUMENT. Use LINEAR for the higher precision candidate.
 * Original Q12 APIs, exponential Q30, and fixed-head numeric version remain
 * unchanged. Link with function/data sections + GC to discard unused tables.
 */
int32_t npu_fixed_sigmoid_q16(int32_t x, npu_fixed_lut_mode mode,
                            npu_fixed_status *status);
int32_t npu_fixed_tanh_q16(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *status);
int32_t npu_fixed_silu_q16(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *status);
int32_t npu_fixed_softplus_q16(int32_t x, npu_fixed_lut_mode mode,
                             npu_fixed_status *status);

/* Input: int32 Q11, intended for a max-subtracted softmax argument in [-16,0].
 * Output: uint32 Q30 exp(x), [121,1073741824]. Out-of-domain inputs clamp to
 * -16 or 0 and set DOMAIN_CLAMP (values below -16 do NOT flush to zero).
 * Knot spacing 1/256. LINEAR absolute error <=2050 Q30 units; NEAREST
 * <=2097153 units. Relative error near -16 is dominated by Q30 quantization
 * (conservative <=0.84% for LINEAR); this is an exponential primitive, not a
 * normalization routine. It does not subtract a max or sum/divide outputs.
 */
uint32_t npu_fixed_exp_negative_q30(int32_t x_q11, npu_fixed_lut_mode mode,
                                  npu_fixed_status *status);

/* uint32 phase means phase/2^32 turns, wrapping modulo one turn. Output int32
 * Q15 is [-32768,32768], so +1 is exactly representable (not int16 Q1.15).
 * Quarter-wave LUT, 1024 intervals per quadrant; no domain clamp is needed.
 * LINEAR abs error <=2 Q15 units; NEAREST <=26. No phase-input quantization
 * error is included. Both output pointers must be non-NULL; on invalid input
 * neither is written. Aliased output pointers are invalid as well.
 */
uint32_t npu_fixed_sincos_q15(uint32_t phase, npu_fixed_lut_mode mode,
                            int32_t *sine_q15, int32_t *cosine_q15,
                            npu_fixed_status *status);

/* Input real v_i = input[i] * 2^input_exponent, with signed int16 input.
 * q_i = RNE(input[i]*127/maxabs), output int8 in [-127,127]; -32768 is safe.
 * Returned scale represents EXACTLY maxabs*2^input_exponent/127, with no
 * intermediate binary rounding. The exponent has units of binary powers of
 * the input integer LSB, NOT Q-format fractional bits (Q12 => exponent -12).
 * All-zero or empty input returns maxabs=0, preserved exponent, zero outputs,
 * and ZERO_VECTOR. Empty input permits NULL data pointers. For nonempty input
 * all pointers are required; input/output storage must not overlap.
 */
typedef struct {
    uint16_t maxabs;          /* 0..32768, positive unsigned magnitude */
    int32_t input_exponent;  /* full int32 exponent range is accepted */
} npu_fixed_a8_scale;
uint32_t npu_fixed_quantize_a8(const int16_t *input, size_t count,
                             int32_t input_exponent, int8_t *output,
                             npu_fixed_a8_scale *scale,
                             npu_fixed_status *status);

/* One signed INT32 dot-product partial times a finite IEEE binary16 weight
 * scale bit pattern times the exact A8 activation scale above, converted RNE
 * to int32 Q16. No FP16 arithmetic or intermediate FP32 rounding occurs.
 * Negative finite weight scales and signed zero are supported. Nonfinite
 * weight scales or invalid descriptors return zero and flag an error. Finite
 * overflow saturates to INT32_MIN/MAX; nonzero underflow returns zero.
 * Invalid scale descriptors: NULL or maxabs>32768. maxabs=0 is valid.
 */
int32_t npu_fixed_partial_fp16_q16(int32_t partial, uint16_t weight_scale_bits,
                                 const npu_fixed_a8_scale *activation_scale,
                                 npu_fixed_status *status);

/* Each term is independently rounded to int64 Q16, accumulated in array order
 * with saturating int64 addition, then biased by bias_q16 and saturated to
 * int32 Q16. This declared order is numeric version 1, not original FP32 bit
 * identity. Int64 intermediate saturation sets SATURATED and can affect later
 * cancellation. Each term's rounding error <=0.5 Q16 unit without saturation;
 * count terms can accumulate count/2 Q16 units of conversion rounding error.
 * An invalid/nonfinite term aborts with output untouched. Output may alias
 * neither inputs nor scales. count=0 returns bias and permits NULL arrays.
 * Return flags describe this call, even if status already contains old flags.
 */
uint32_t npu_fixed_reduce_fp16_q16(const int32_t *partials,
                                 const uint16_t *weight_scale_bits,
                                 size_t count,
                                 const npu_fixed_a8_scale *activation_scale,
                                 int32_t bias_q16, int32_t *output_q16,
                                 npu_fixed_status *status);

/* Optional storage bit-pattern conversions, all RNE to int32 Q16. IEEE
 * binary16, bfloat16, and binary32 bit fields only; no C real-number types.
 * Subnormals are decoded exactly before output rounding. NaN returns 0 and
 * NONFINITE; +/-infinity returns INT32_{MAX,MIN} and NONFINITE|SATURATED.
 * Signed zero becomes integer zero. Finite overflow saturates; finite nonzero
 * values rounded to zero set UNDERFLOW. Maximum unsaturated error 0.5/65536.
 */
int32_t npu_fixed_fp16_bits_to_q16(uint16_t bits, npu_fixed_status *status);
int32_t npu_fixed_bf16_bits_to_q16(uint16_t bits, npu_fixed_status *status);
int32_t npu_fixed_f32_bits_to_q16(uint32_t bits, npu_fixed_status *status);

#ifdef __cplusplus
}
#endif
#endif
