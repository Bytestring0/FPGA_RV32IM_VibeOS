#include "npu_runtime.h"

static inline void barrier(void) {
#if defined(__riscv)
    __asm__ volatile("fence iorw,iorw" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}
int ag_cache_maintain(uint32_t limit) {
#if defined(NPU_TEST_SKIP_CACHE)
    (void)limit;
    return 0; /* negative-control build only */
#elif defined(__riscv)
    volatile uint32_t *const status=(volatile uint32_t *)0x10000200u;
    volatile uint32_t *const control=(volatile uint32_t *)0x10000204u;
    barrier();
    uint32_t s=*status;
    if (!(s&1u) || (s&0x12u)) return -1;
    if (!(s&0x20u)) { barrier(); return 0; }
    while ((*control&1u) && limit) --limit;
    if (!limit) return -2;
    *control=1;
    barrier();
    while (limit--) {
        uint32_t c=*control;
        if (c&2u) return -1;
        if (!(c&1u)) { barrier(); return 0; }
    }
    return -2;
#else
    (void)limit;
    return -3; /* host is not a proof of AG DMA/cache synchronization */
#endif
}
int ag_dma_publish(uint32_t limit) { return ag_cache_maintain(limit); }
int ag_dma_acquire(uint32_t limit) { return ag_cache_maintain(limit); }

static uint32_t float_bits(float f) { union {float f;uint32_t u;} v;v.f=f;return v.u; }
static float from_bits(uint32_t u) { union {float f;uint32_t u;} v;v.u=u;return v.f; }
static int finite_float(float f) { return (float_bits(f)&0x7f800000u)!=0x7f800000u; }
float npu_binary16_to_float(uint16_t h) {
    uint32_t sign=(uint32_t)(h&0x8000u)<<16;
    uint32_t exponent=(h>>10)&31u, fraction=h&1023u;
    if (exponent==31u) return from_bits(sign|0x7f800000u|(fraction<<13));
    if (!exponent) {
        if (!fraction) return from_bits(sign);
        int e=-14;
        while (!(fraction&1024u)) { fraction<<=1; --e; }
        return from_bits(sign|((uint32_t)(e+127)<<23)|((fraction&1023u)<<13));
    }
    return from_bits(sign|((exponent+112u)<<23)|(fraction<<13));
}
int npu_reduce_strided(const void *records, size_t buffer_bytes,
                       uint32_t rows, uint32_t cols, uint32_t stride,
                       const uint16_t *scales, float a_scale,
                       const float *bias, float *out) {
    if (!records || !scales || !out || !rows || !cols || rows>65535u ||
        cols>4096u || !finite_float(a_scale) || a_scale<0.0f ||
        ((uintptr_t)records&3u) || (stride&3u)) return -1;
    uint32_t groups=(cols+127u)/128u;
    uint64_t required=(uint64_t)(rows-1u)*stride+(uint64_t)groups*sizeof(npu_partial);
    if (stride<groups*sizeof(npu_partial) || required>buffer_bytes) return -1;
    for (uint32_t r=0;r<rows;r++) {
        const npu_partial *p=(const npu_partial *)((const uint8_t *)records+(size_t)r*stride);
        if (bias && !finite_float(bias[r])) return -1;
        for (uint32_t g=0;g<groups;g++) {
            float w=npu_binary16_to_float(scales[(size_t)r*groups+g]);
            if (p[g].tag!=(r|(g<<16)) || !finite_float(w) || w<0.0f) return -1;
        }
    }
    for (uint32_t r=0;r<rows;r++) {
        const npu_partial *p=(const npu_partial *)((const uint8_t *)records+(size_t)r*stride);
        float sum=0.0f;
        for (uint32_t g=0;g<groups;g++) {
            float term=(float)p[g].partial*npu_binary16_to_float(scales[(size_t)r*groups+g]);
            sum=sum+term;
        }
        float value=sum*a_scale;
        if (bias) value=value+bias[r];
        if (!finite_float(value)) return -2;
        out[r]=value;
    }
    return 0;
}
int npu_reduce_partials(const npu_partial *p, size_t count,
                        uint32_t rows, uint32_t cols,
                        const uint16_t *scales, float a_scale,
                        const float *bias, float *out) {
    if (!cols || cols>4096u || count!=(size_t)rows*((cols+127u)/128u)) return -1;
    return npu_reduce_strided(p,count*sizeof(*p),rows,cols,((cols+127u)/128u)*sizeof(*p),
                              scales,a_scale,bias,out);
}
int npu_quantize_activation(const float *in,uint32_t n,int8_t *out,float *scale) {
    if (!in || !out || !scale || !n || n>4096u) return -1;
    float maximum=0.0f;
    for (uint32_t i=0;i<n;i++) {
        float x=in[i];
        if (!finite_float(x)) return -1;
        if (x<0.0f) x=-x;
        if (x>maximum) maximum=x;
    }
    float s=maximum==0.0f ? 1.0f : maximum/127.0f;
    if (!finite_float(s) || s<=0.0f) return -1;
    for (uint32_t i=0;i<n;i++) {
        float x=in[i]/s;
        if (x>127.0f) x=127.0f;
        if (x<-127.0f) x=-127.0f;
        int negative=x<0.0f;
        float magnitude=negative ? -x : x;
        int q=(int)magnitude;
        float remainder=magnitude-(float)q;
        if (remainder>0.5f || (remainder==0.5f && (q&1))) ++q;
        out[i]=(int8_t)(negative ? -q : q);
    }
    *scale=s;
    return 0;
}
