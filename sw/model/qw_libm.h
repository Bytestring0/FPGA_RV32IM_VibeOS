#ifndef QW_LIBM_H
#define QW_LIBM_H
/* Portable numerical policy v1: no target libc/libm or hardware F extension.
 * Trig domain is the text model's |position*frequency| <=1024. All outputs
 * are binary32; binary64 is used only for trig argument reduction. */
float qwm_exp(float);
float qwm_log1p(float);
float qwm_sqrt(float);
float qwm_sin(float);
float qwm_cos(float);
float qwm_pow(float,float);
static inline float qwm_infinity(void){union{unsigned u;float f;}v;v.u=0x7f800000u;return v.f;}
#define expf qwm_exp
#define log1pf qwm_log1p
#define sqrtf qwm_sqrt
#define sinf qwm_sin
#define cosf qwm_cos
#define powf qwm_pow
#ifndef INFINITY
#define INFINITY (qwm_infinity())
#endif
#endif
