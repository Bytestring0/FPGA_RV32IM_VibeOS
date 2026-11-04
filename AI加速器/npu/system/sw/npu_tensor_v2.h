#ifndef NPU_TENSOR_V2_H
#define NPU_TENSOR_V2_H
#include <stddef.h>
#include <stdint.h>
#include "npu_fixed_math.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NPU_TENSOR_ID UINT32_C(0x4e505632)
#define NPU_TENSOR_NUMERIC_VERSION UINT32_C(0x00020000)
#define NPU_TENSOR_DDR_LIMIT UINT32_C(0x40000000)
enum { NPU_TENSOR_COPY=1,NPU_TENSOR_ADD,NPU_TENSOR_MUL,NPU_TENSOR_SIGMOID,
 NPU_TENSOR_SILU,NPU_TENSOR_SOFTPLUS,NPU_TENSOR_EXP_NEG,NPU_TENSOR_RMSNORM,
 NPU_TENSOR_SOFTMAX,NPU_TENSOR_ROPE,NPU_TENSOR_REDUCE,NPU_TENSOR_DWCONV,
 NPU_TENSOR_DELTA,NPU_TENSOR_ATTENTION,NPU_TENSOR_QUANT_A8,NPU_TENSOR_ARGMAX,
 NPU_TENSOR_MATVEC_Q16,NPU_TENSOR_VECMAT_Q16,NPU_TENSOR_REDUCE_RAW_W4,
 NPU_TENSOR_DELTA_Q24, NPU_TENSOR_REDUCE_RAW_W4_Q46, NPU_TENSOR_RMSNORM_PRECISE };
enum { NPU_TENSOR_OK=0,NPU_TENSOR_ERR_ARGUMENT=1,NPU_TENSOR_ERR_OPCODE=2,
 NPU_TENSOR_ERR_DIMENSION=3,NPU_TENSOR_ERR_PARAMETER=4,NPU_TENSOR_ERR_STRIDE=5,
 NPU_TENSOR_ERR_ALIGNMENT=6,NPU_TENSOR_ERR_RANGE=7,NPU_TENSOR_ERR_OVERLAP=8,
 NPU_TENSOR_ERR_MAPPING=9,NPU_TENSOR_ERR_TAG=12 };
enum { NPU_TENSOR_READ=1,NPU_TENSOR_WRITE=2 };
typedef struct {
 uint32_t opcode,n,m,k,a,b,c,out,state,param0,param1,param2;
 uint32_t stride_a,stride_b,stride_out,flags;
} npu_tensor_job;
typedef struct { uint32_t offset,bytes,access; } npu_tensor_span;
typedef struct {
 npu_tensor_span spans[6];
 uint32_t span_count,scratch_bytes;
} npu_tensor_layout;
typedef struct { uint8_t *data; size_t capacity; uint32_t ddr_base; } npu_tensor_mapping;
typedef struct { uint32_t error,result; npu_fixed_status numeric; } npu_tensor_stats;
/* Query emits spans in A,B,C,OUT,STATE order, skipping unused/empty spans.
 * READ inputs may alias each other. No write span may overlap another span;
 * only the single READ|WRITE state span is intentionally read-modify-write.
 * Nonempty bases are 32B-aligned. All scalar strides and flags must be zero,
 * except REDUCE_RAW_W4 stride_a as explicitly described below.
 * Ignored fields are permitted; op-specific booleans must be 0/1. n is 1..4096.
 * Error precedence: argument, opcode, dimension, parameter, stride/flags,
 * alignment, DDR range, overlap, mapping. Query leaves layout untouched on
 * error, including native descriptor/layout alias. Reference snapshots job
 * and mapping, rejects stats overlapping ANY mapped memory/job/mapping before
 * writing stats, and validates every DDR span before ANY output/state write.
 * Null job/mapping or native metadata aliases return without stats writes.
 * For other safe stats, validation failures publish error and zero numeric.
 * Reference accesses only mapping bytes (never MMIO); no malloc or floats.
 * Success may report sticky saturation/underflow/clamp/zero flags. All signed
 * accumulations saturate at int64 at each ascending-index addition. Every
 * nonzero arithmetic rescale rounded to zero reports UNDERFLOW (LUT zeros do
 * not). Saturation counts each int64/int32 clipping stage. Stats are per-call.
 * scratch_bytes conservatively bounds local arithmetic array bytes, excluding
 * call frames: output[4096], temporary[4096], state[16384] = 98304 bytes.
 * REDUCE_RAW_W4_Q46(21) is additive: same layout/guards as19, with up to16
 * extra coefficient bits. For p=mant*maximum and shift=e?e-11:-10, choose
 * extra=clamp(36-floor(log2(p))-shift,0,16), or0 if p=0. Coefficient is
 * sat32(RNE(p*2^(shift+extra)/127)); terms RNE(partial*coef/2^(14+extra)).
 * Extra bits are reduced before overflow; legacy19 is unchanged. Quality
 * must be measured per model. Q46 names the maximum fractional precision.
 * This experimental policy is not model-quality or hardware validation.
 *
 * RMSNORM_PRECISE(22): same validation/layout as8; mean and epsilon unchanged.
 * For positive radicand r, f=min(8,floor(clz64(r)/2)); root=floor(sqrt(r*2^(2*f))).
 * reciprocal=RNE(2^(48+f)/root); norm=RNE(x*reciprocal/2^32), followed by
 * the same weight/center and sat32 as8. One reciprocal division per job replaces
 * one division per element. Opcode8 keeps its exact legacy rounding.
 * Numeric operation order (storage is signed int32 unless stated):
 * COPY/ADD operate Q16; MUL RNE(a*b/2^16). SIGMOID/SILU/SOFTPLUS use
 * npu_fixed_*_q16 LINEAR profile 0x10001 unchanged. EXP_NEG first RNE(Q16/32)
 * to Q11, clamps [-32768,0], existing expQ30 LINEAR, then RNE(/2^14) to Q16.
 * RMSNORM mean=RNE(sum(a*a)/n) computed exactly without overflow, add unsigned
 * Q32 epsilon param0 (zero selects4295), integer sqrt FLOOR; norm=RNE(a*65536/
 * root), out=sat32(RNE(norm*(b+(param1?65536:0))/65536)); param1 is boolean.
 * SOFTMAX max-subtracts in int64, converts/clamps Q11 as EXP_NEG, then each
 * out_i=RNE(expQ30_i*65536/sum(expQ30)); no residual sum correction.
 * ROPE pairs first/second halves; combine two signed products with sat64,
 * then RNE(/2^30), then sat32. REDUCE term=RNE(partial*coefficientQ30/2^14),
 * ascending sat64 term accumulation, optional c bias AFTER all terms.
 * DWCONV state is [channel,k-1] oldest first; ascending taps include new a
 * last, sum raw Q32 products with sat64, RNE(/65536), sat32; shift state once.
 * DELTA state[n,n]Q24: first sat32(RNE(state*decayQ30/2^30)); mem_j is
 * sat32(RNE(sat64 ascending sum_i(state_ij*k_i)/2^24)); delta_j is
 * sat32(RNE((v_j-mem_j)*betaQ30/2^30)); updated state is
 * sat32(state_ij+RNE(k_i*delta_j/256)); output_j is sat32(RNE(ascending sat64
 * sum_i(updated_state_ij*q_i)/2^24)). decay/beta are unsigned [0,2^30].
 * ATTENTION n<=32,k<=256: ascending sat64 dot, exact wide dot*scaleQ30 with
 * RNE(/2^46), sat32 score, above SOFTMAX, ascending sat64 probQ16*valueQ16
 * dot, RNE(/65536), sat32 output; param0 scale is unsigned [0,INT32_MAX].
 * QUANT_A8 out bytes are RNE(a*127/maxabs) in[-127,127], result=maxabs;
 * zero vector emits zero bytes and ZERO_VECTOR. ARGMAX returns first tie index
 * both as one output word and result. MATVEC17: a[n,k],b[k],out[n],n<=32,
 * k<=4096. VECMAT18: a[n],b[n,k],out[k],n<=32,k<=256. Both use ascending
 * sat64 dot, RNE(/65536), sat32. They ignore param0; score scaling is explicit.
 * Ignored descriptor fields are permitted. Empty DWCONV state (k=1) has no
 * alignment/range requirement. Input read spans may coincide exactly/partly.
 * DELTA_Q24(20) has exactly the same inputs/state/update order as DELTA13;
 * only its final dot rounds by2^16, producing signed Q24 output. Its post
 * RMSNORM epsilon is in squared input units: Q24 uses Q48, so a Q16 epsilon
 * of4295 becomes4295<<16=281477120 (fits uint32). Norm output remains Q16.
 * This avoids erasing sub-Q16 recurrent signals before normalization.
 * Legacy DELTA13 output and every existing opcode remain unchanged.
 * REDUCE_RAW_W4(19): n<=4096,m<=32,a contains raw8B little-endian records
 * (uint16 row,uint16 group,int32 partial), stride_a>=m*8 and multiple32.
 * b is dense positive finite nonzero FP16 LE scales[n,m], param0 unsigned
 * activation maxabsQ16<=0x80000000; c/param1/param2 must be zero. A span is
 * (n-1)*stride_a+m*8 bytes and B span n*m*2. All row/group tags and scales
 * are scanned in ascending order before computation/output: each record
 * first checks tag(row==r,group==g), then its scale. Bad tag returns12;
 * bad scale(sign/zero/exp31) returns4 plus INVALID_ARGUMENT,invalid_inputs=1.
 * For exp e and mant=(e?1024:0)+fraction, shift=e?e-11:-10, coefficient is
 * sat32(RNE(mant*param0*2^shift/127)); nonzero coefficient rounded to zero
 * flags UNDERFLOW, clipping flags SATURATED once. Each raw partial then uses
 * RNE(partial*coefficient/2^14), ascending sat64 sum, final sat32 Q16.
 */
uint32_t npu_tensor_query(const npu_tensor_job *job,npu_tensor_layout *layout);
uint32_t npu_tensor_reference(const npu_tensor_job *job,
 const npu_tensor_mapping *mapping,npu_tensor_stats *stats);
#ifdef __cplusplus
}
#endif
#endif
