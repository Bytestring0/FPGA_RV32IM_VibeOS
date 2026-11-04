#include "npu_fixed_math.h"
#include "npu_fixed_lut_generated.h"
#include "npu_fixed_lut_q16_generated.h"

#if NPU_FIXED_Q16_LUT_NUMERIC_VERSION != NPU_FIXED_LUT_Q16_NUMERIC_VERSION
#error "Q16 activation numeric version does not match its generated LUTs"
#endif

#if NPU_FIXED_NUMERIC_VERSION != NPU_FIXED_LUT_NUMERIC_VERSION
#error "Fixed math numeric version does not match the generated LUTs"
#endif

static uint32_t counter_add(uint32_t a, uint32_t b)
{
    return UINT32_MAX - a < b ? UINT32_MAX : a + b;
}

static void flag(npu_fixed_status *s, uint32_t bits)
{
    if (s == NULL) return;
    s->status |= bits;
    if ((bits & NPU_FIXED_STATUS_DOMAIN_CLAMP) != 0)
        s->domain_clamps = counter_add(s->domain_clamps, 1);
    if ((bits & NPU_FIXED_STATUS_SATURATED) != 0)
        s->saturations = counter_add(s->saturations, 1);
    if ((bits & NPU_FIXED_STATUS_UNDERFLOW) != 0)
        s->underflows = counter_add(s->underflows, 1);
    if ((bits & NPU_FIXED_STATUS_ERROR_MASK) != 0)
        s->invalid_inputs = counter_add(s->invalid_inputs, 1);
    if ((bits & NPU_FIXED_STATUS_ZERO_VECTOR) != 0)
        s->zero_vectors = counter_add(s->zero_vectors, 1);
}

void npu_fixed_status_clear(npu_fixed_status *s)
{
    if (s == NULL) return;
    s->status = 0;
    s->domain_clamps = 0;
    s->saturations = 0;
    s->underflows = 0;
    s->invalid_inputs = 0;
    s->zero_vectors = 0;
}

static void merge_status(npu_fixed_status *dst, const npu_fixed_status *src)
{
    if (dst == NULL) return;
    dst->status |= src->status;
    dst->domain_clamps = counter_add(dst->domain_clamps, src->domain_clamps);
    dst->saturations = counter_add(dst->saturations, src->saturations);
    dst->underflows = counter_add(dst->underflows, src->underflows);
    dst->invalid_inputs = counter_add(dst->invalid_inputs, src->invalid_inputs);
    dst->zero_vectors = counter_add(dst->zero_vectors, src->zero_vectors);
}

static int mode_ok(npu_fixed_lut_mode mode, npu_fixed_status *s)
{
    if (mode == NPU_FIXED_LUT_NEAREST || mode == NPU_FIXED_LUT_LINEAR)
        return 1;
    flag(s, NPU_FIXED_STATUS_INVALID_ARGUMENT);
    return 0;
}

/* Signed division by a small positive divisor, including INT64_MIN. */
static int64_t div_rne(int64_t x, uint32_t d)
{
    uint64_t mag = x < 0 ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;
    uint64_t q = mag / d;
    uint64_t r = mag % d;
    if (r > (uint64_t)d - r || (r == (uint64_t)d - r && (q & 1) != 0))
        ++q;
    if (x >= 0) return (int64_t)q;
    if (q == (UINT64_C(1) << 63)) return INT64_MIN;
    return -(int64_t)q;
}

static uint32_t magnitude_i32(int32_t x)
{
    return x < 0 ? (uint32_t)(-(int64_t)x) : (uint32_t)x;
}

static uint32_t nearest_index(uint32_t coord, uint32_t frac_bits)
{
    uint32_t idx = coord >> frac_bits;
    uint32_t mask = (UINT32_C(1) << frac_bits) - 1;
    uint32_t r = coord & mask;
    uint32_t half = UINT32_C(1) << (frac_bits - 1);
    if (r > half || (r == half && (idx & 1) != 0)) ++idx;
    return idx;
}

static int32_t lookup_u16(const uint16_t *table, uint32_t coord,
                          uint32_t frac_bits, uint32_t endpoint,
                          npu_fixed_lut_mode mode)
{
    uint32_t idx, frac;
    int64_t delta;
    if (mode == NPU_FIXED_LUT_NEAREST)
        return (int32_t)table[nearest_index(coord, frac_bits)];
    idx = coord >> frac_bits;
    if (idx == endpoint) return table[idx];
    frac = coord & ((UINT32_C(1) << frac_bits) - 1);
    delta = (int64_t)table[idx + 1] - table[idx];
    /* Round the absolute interpolated result. Rounding delta separately would
     * give the wrong tie parity when table[idx] is odd. */
    return (int32_t)div_rne(((int64_t)table[idx] << frac_bits) + delta * frac,
                            UINT32_C(1) << frac_bits);
}

static int32_t lookup_u32(const uint32_t *table, uint32_t coord,
                          uint32_t frac_bits, uint32_t endpoint,
                          npu_fixed_lut_mode mode)
{
    uint32_t idx, frac;
    int64_t delta;
    if (mode == NPU_FIXED_LUT_NEAREST)
        return (int32_t)table[nearest_index(coord, frac_bits)];
    idx = coord >> frac_bits;
    if (idx == endpoint) return (int32_t)table[idx];
    frac = coord & ((UINT32_C(1) << frac_bits) - 1);
    delta = (int64_t)table[idx + 1] - table[idx];
    return (int32_t)div_rne(((int64_t)table[idx] << frac_bits) + delta * frac,
                            UINT32_C(1) << frac_bits);
}

int32_t npu_fixed_sigmoid_q12(int32_t x, npu_fixed_lut_mode mode,
                            npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 65536) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? 0 : NPU_FIXED_Q12_ONE;
    }
    y = lookup_u16(npu_fixed_lut_sigmoid_q12, mag, 6, 1024, mode);
    return x < 0 ? NPU_FIXED_Q12_ONE - y : y;
}

int32_t npu_fixed_tanh_q12(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 65536) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? -NPU_FIXED_Q12_ONE : NPU_FIXED_Q12_ONE;
    }
    y = lookup_u16(npu_fixed_lut_tanh_q12, mag, 6, 1024, mode);
    return x < 0 ? -y : y;
}

int32_t npu_fixed_silu_q12(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 65536) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? 0 : x;
    }
    if (mode == NPU_FIXED_LUT_NEAREST) mag = nearest_index(mag, 6) << 6;
    y = lookup_u32(npu_fixed_lut_silu_q12, mag, 6, 1024, mode);
    return x < 0 ? y - (int32_t)mag : y;
}

int32_t npu_fixed_softplus_q12(int32_t x, npu_fixed_lut_mode mode,
                             npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 65536) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? 0 : x;
    }
    if (mode == NPU_FIXED_LUT_NEAREST) mag = nearest_index(mag, 6) << 6;
    y = lookup_u32(npu_fixed_lut_softplus_q12, mag, 6, 1024, mode);
    return x < 0 ? y - (int32_t)mag : y;
}

int32_t npu_fixed_sigmoid_q16(int32_t x, npu_fixed_lut_mode mode,
                            npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 1048576) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? 0 : NPU_FIXED_Q16_ONE;
    }
    y = lookup_u32(npu_fixed_lut_sigmoid_q16, mag, 10, 1024, mode);
    return x < 0 ? NPU_FIXED_Q16_ONE - y : y;
}

int32_t npu_fixed_tanh_q16(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 1048576) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? -NPU_FIXED_Q16_ONE : NPU_FIXED_Q16_ONE;
    }
    y = lookup_u32(npu_fixed_lut_tanh_q16, mag, 10, 1024, mode);
    return x < 0 ? -y : y;
}

int32_t npu_fixed_silu_q16(int32_t x, npu_fixed_lut_mode mode,
                         npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 1048576) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? 0 : x;
    }
    if (mode == NPU_FIXED_LUT_NEAREST) mag = nearest_index(mag, 10) << 10;
    y = lookup_u32(npu_fixed_lut_silu_q16, mag, 10, 1024, mode);
    return x < 0 ? y - (int32_t)mag : y;
}

int32_t npu_fixed_softplus_q16(int32_t x, npu_fixed_lut_mode mode,
                             npu_fixed_status *s)
{
    uint32_t mag;
    int32_t y;
    if (!mode_ok(mode, s)) return 0;
    mag = magnitude_i32(x);
    if (mag > 1048576) {
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
        return x < 0 ? 0 : x;
    }
    if (mode == NPU_FIXED_LUT_NEAREST) mag = nearest_index(mag, 10) << 10;
    y = lookup_u32(npu_fixed_lut_softplus_q16, mag, 10, 1024, mode);
    return x < 0 ? y - (int32_t)mag : y;
}

uint32_t npu_fixed_exp_negative_q30(int32_t x, npu_fixed_lut_mode mode,
                                  npu_fixed_status *s)
{
    if (!mode_ok(mode, s)) return 0;
    if (x < -32768) {
        x = -32768;
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
    } else if (x > 0) {
        x = 0;
        flag(s, NPU_FIXED_STATUS_DOMAIN_CLAMP);
    }
    return (uint32_t)lookup_u32(npu_fixed_lut_exp_negative_q30,
                                (uint32_t)(-x), 3, 4096, mode);
}

static int32_t sine_phase(uint32_t phase, npu_fixed_lut_mode mode)
{
    uint32_t quadrant = phase >> 30;
    uint32_t within = phase & UINT32_C(0x3fffffff);
    uint32_t coord = (quadrant & 1) != 0 ?
        UINT32_C(0x40000000) - within : within;
    int32_t y = lookup_u16(npu_fixed_lut_sine_quarter_q15, coord, 20, 1024, mode);
    return (quadrant & 2) != 0 ? -y : y;
}

uint32_t npu_fixed_sincos_q15(uint32_t phase, npu_fixed_lut_mode mode,
                            int32_t *sine, int32_t *cosine,
                            npu_fixed_status *s)
{
    if (sine == NULL || cosine == NULL || sine == cosine) {
        flag(s, NPU_FIXED_STATUS_INVALID_ARGUMENT);
        return NPU_FIXED_STATUS_INVALID_ARGUMENT;
    }
    if (!mode_ok(mode, s)) return NPU_FIXED_STATUS_INVALID_ARGUMENT;
    *sine = sine_phase(phase, mode);
    *cosine = sine_phase(phase + UINT32_C(0x40000000), mode);
    return 0;
}

uint32_t npu_fixed_quantize_a8(const int16_t *input, size_t count,
                             int32_t exponent, int8_t *output,
                             npu_fixed_a8_scale *scale, npu_fixed_status *s)
{
    size_t i;
    uint32_t peak = 0;
    if (scale == NULL || (count != 0 && (input == NULL || output == NULL))) {
        flag(s, NPU_FIXED_STATUS_INVALID_ARGUMENT);
        return NPU_FIXED_STATUS_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; ++i) {
        uint32_t mag = magnitude_i32(input[i]);
        if (mag > peak) peak = mag;
    }
    scale->maxabs = (uint16_t)peak;
    scale->input_exponent = exponent;
    if (peak == 0) {
        for (i = 0; i < count; ++i) output[i] = 0;
        flag(s, NPU_FIXED_STATUS_ZERO_VECTOR);
        return NPU_FIXED_STATUS_ZERO_VECTOR;
    }
    for (i = 0; i < count; ++i)
        output[i] = (int8_t)div_rne((int64_t)input[i] * 127, peak);
    return 0;
}

/* Exact unsigned rational scaling without a wide numerator or signed shifts.
 * mag is at most 2^63, divisor is positive. For nonnegative shift, carry the
 * remainder through binary long division; at most ~70 iterations are reached
 * before overflow for our divisors 1/127, regardless of the input exponent.
 * For negative shift, a denominator beyond uint64 range implies a value <=
 * one half, whose RNE result is zero. limit is 2^63-1 or 2^63.
 */
static uint64_t scaled_rne(uint64_t mag, int64_t shift, uint32_t divisor,
                           uint64_t limit, npu_fixed_status *s)
{
    uint64_t q, r, d = divisor;
    if (mag == 0) return 0;
    if (shift < 0) {
        uint64_t right = (uint64_t)(-(shift + 1)) + 1;
        if (right >= 64 || d > (UINT64_MAX >> (unsigned)right)) {
            flag(s, NPU_FIXED_STATUS_UNDERFLOW);
            return 0;
        }
        d <<= (unsigned)right;
        q = mag / d;
        r = mag % d;
    } else {
        q = mag / d;
        r = mag % d;
        while (shift > 0) {
            uint64_t bit;
            r *= 2; /* d<=127 in this branch, so the carry fits. */
            bit = r >= d ? 1 : 0;
            if (bit != 0) r -= d;
            if (q > (limit - bit) / 2) {
                flag(s, NPU_FIXED_STATUS_SATURATED);
                return limit;
            }
            q = q * 2 + bit;
            --shift;
        }
    }
    if (r > d - r || (r == d - r && (q & 1) != 0)) {
        if (q >= limit) {
            flag(s, NPU_FIXED_STATUS_SATURATED);
            return limit;
        }
        ++q;
    }
    if (q > limit) {
        flag(s, NPU_FIXED_STATUS_SATURATED);
        return limit;
    }
    if (q == 0) flag(s, NPU_FIXED_STATUS_UNDERFLOW);
    return q;
}

static int64_t signed_scaled(uint64_t mag, int negative, int64_t shift,
                             uint32_t divisor, npu_fixed_status *s)
{
    uint64_t limit = negative ? (UINT64_C(1) << 63) : (uint64_t)INT64_MAX;
    uint64_t q = scaled_rne(mag, shift, divisor, limit, s);
    if (!negative) return (int64_t)q;
    if (q == (UINT64_C(1) << 63)) return INT64_MIN;
    return -(int64_t)q;
}

static int32_t saturate_q16(int64_t x, npu_fixed_status *s)
{
    if (x > INT32_MAX) {
        flag(s, NPU_FIXED_STATUS_SATURATED);
        return INT32_MAX;
    }
    if (x < INT32_MIN) {
        flag(s, NPU_FIXED_STATUS_SATURATED);
        return INT32_MIN;
    }
    return (int32_t)x;
}

static int scale_valid(const npu_fixed_a8_scale *scale, npu_fixed_status *s)
{
    if (scale == NULL || scale->maxabs > 32768) {
        flag(s, NPU_FIXED_STATUS_INVALID_ARGUMENT);
        return 0;
    }
    return 1;
}

static int64_t partial_q16_wide(int32_t partial, uint16_t bits,
                                const npu_fixed_a8_scale *scale,
                                npu_fixed_status *s)
{
    uint32_t e = ((uint32_t)bits >> 10) & 31;
    uint32_t mantissa = bits & 1023;
    int32_t power;
    uint64_t mag;
    int negative;
    if (!scale_valid(scale, s)) return 0;
    if (e == 31) {
        flag(s, NPU_FIXED_STATUS_NONFINITE);
        return 0;
    }
    if (e == 0) {
        power = -24;
    } else {
        mantissa += 1024;
        power = (int32_t)e - 25;
    }
    mag = (uint64_t)magnitude_i32(partial) * mantissa * scale->maxabs;
    negative = (partial < 0) != ((bits & 32768) != 0);
    return signed_scaled(mag, negative,
                          (int64_t)power + scale->input_exponent + 16,
                          127, s);
}

int32_t npu_fixed_partial_fp16_q16(int32_t partial, uint16_t bits,
                                 const npu_fixed_a8_scale *scale,
                                 npu_fixed_status *s)
{
    return saturate_q16(partial_q16_wide(partial, bits, scale, s), s);
}

static int64_t add_saturating_i64(int64_t a, int64_t b, npu_fixed_status *s)
{
    if (b > 0 && a > INT64_MAX - b) {
        flag(s, NPU_FIXED_STATUS_SATURATED);
        return INT64_MAX;
    }
    if (b < 0 && a < INT64_MIN - b) {
        flag(s, NPU_FIXED_STATUS_SATURATED);
        return INT64_MIN;
    }
    return a + b;
}

uint32_t npu_fixed_reduce_fp16_q16(const int32_t *partials,
                                 const uint16_t *weight_scales, size_t count,
                                 const npu_fixed_a8_scale *activation_scale,
                                 int32_t bias_q16, int32_t *output,
                                 npu_fixed_status *s)
{
    npu_fixed_status local;
    int64_t sum = 0;
    size_t i;
    npu_fixed_status_clear(&local);
    if (output == NULL || (count != 0 && (partials == NULL || weight_scales == NULL))) {
        flag(&local, NPU_FIXED_STATUS_INVALID_ARGUMENT);
    } else if (scale_valid(activation_scale, &local)) {
        for (i = 0; i < count; ++i) {
            int64_t term = partial_q16_wide(partials[i], weight_scales[i],
                                            activation_scale, &local);
            if ((local.status & NPU_FIXED_STATUS_ERROR_MASK) != 0) break;
            sum = add_saturating_i64(sum, term, &local);
        }
        if ((local.status & NPU_FIXED_STATUS_ERROR_MASK) == 0) {
            sum = add_saturating_i64(sum, bias_q16, &local);
            *output = saturate_q16(sum, &local);
        }
    }
    merge_status(s, &local);
    return local.status;
}

/* All IEEE decoders produce an integer significand and a binary exponent.
 * The sum with 16 maps the represented real directly into Q16 units. */
static int32_t decode_to_q16(uint32_t bits, uint32_t fraction_bits,
                             uint32_t exponent_bits, int32_t bias,
                             npu_fixed_status *s)
{
    uint32_t fraction_mask = (UINT32_C(1) << fraction_bits) - 1;
    uint32_t exponent_mask = (UINT32_C(1) << exponent_bits) - 1;
    uint32_t fraction = bits & fraction_mask;
    uint32_t exponent = (bits >> fraction_bits) & exponent_mask;
    int negative = ((bits >> (fraction_bits + exponent_bits)) & 1) != 0;
    int32_t power;
    uint64_t mantissa = fraction;
    if (exponent == exponent_mask) {
        if (fraction != 0) {
            flag(s, NPU_FIXED_STATUS_NONFINITE);
            return 0;
        }
        flag(s, NPU_FIXED_STATUS_NONFINITE | NPU_FIXED_STATUS_SATURATED);
        return negative ? INT32_MIN : INT32_MAX;
    }
    if (exponent == 0) {
        power = 1 - bias - (int32_t)fraction_bits;
    } else {
        power = (int32_t)exponent - bias - (int32_t)fraction_bits;
        mantissa |= UINT64_C(1) << fraction_bits;
    }
    if (!mantissa) return 0;
    int32_t shift = power + 16;
    uint32_t magnitude = (uint32_t)mantissa, rounded;
    if (shift >= 0) {
        uint32_t limit32 = negative ? UINT32_C(0x80000000) : INT32_MAX;
        if (shift >= 32 || magnitude > (limit32 >> (unsigned)shift)) {
            /* Keep both clipping events of the original int64 -> int32 path. */
            uint64_t limit64 = negative ? (UINT64_C(1) << 63) : INT64_MAX;
            if (shift >= 64 || mantissa > (limit64 >> (unsigned)shift))
                flag(s, NPU_FIXED_STATUS_SATURATED);
            flag(s, NPU_FIXED_STATUS_SATURATED);
            return negative ? INT32_MIN : INT32_MAX;
        }
        rounded = magnitude << (unsigned)shift;
    } else {
        unsigned right = (unsigned)-shift;
        if (right >= 32) rounded = 0;
        else {
            uint32_t remainder = magnitude & ((UINT32_C(1) << right) - 1);
            uint32_t half = UINT32_C(1) << (right - 1);
            rounded = magnitude >> right;
            rounded += remainder > half || (remainder == half && (rounded & 1));
        }
        if (!rounded) flag(s, NPU_FIXED_STATUS_UNDERFLOW);
    }
    return negative ? (int32_t)(0u - rounded) : (int32_t)rounded;
}

int32_t npu_fixed_fp16_bits_to_q16(uint16_t bits, npu_fixed_status *s)
{
    return decode_to_q16(bits, 10, 5, 15, s);
}

int32_t npu_fixed_bf16_bits_to_q16(uint16_t bits, npu_fixed_status *s)
{
    return decode_to_q16(bits, 7, 8, 127, s);
}

int32_t npu_fixed_f32_bits_to_q16(uint32_t bits, npu_fixed_status *s)
{
    return decode_to_q16(bits, 23, 8, 127, s);
}
