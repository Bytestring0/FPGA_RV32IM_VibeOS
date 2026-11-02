#ifndef AG_QW_MATH_H
#define AG_QW_MATH_H
#include <stdint.h>
#include <stddef.h>
float qw_bf16(uint16_t);
int qw_finite(float);
float qw_sigmoid(float);
float qw_silu(float);
void qw_rms(float *out,const float *in,const float *weight,unsigned n,int zero_centered);
void qw_rope(float *x,unsigned head_dim,unsigned rotary_dim,uint32_t position,float theta);
void qw_delta(float *state,float *q,float *k,const float *v,float decay,
 float beta,float *out,unsigned dim);
int qw_reduce(const unsigned char *records,size_t bytes,uint32_t rows,
 uint32_t groups,uint32_t stride,const uint16_t *scales,float sx,float *out);
#endif
