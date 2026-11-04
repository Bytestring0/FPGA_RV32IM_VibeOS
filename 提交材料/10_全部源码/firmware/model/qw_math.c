#include "qw_math.h"
#include "vendor/npu_v1/npu_runtime.h"
#include "qw_libm.h"
float qw_bf16(uint16_t h){union{uint32_t u;float f;}v;v.u=(uint32_t)h<<16;return v.f;}
int qw_finite(float x){union{uint32_t u;float f;}v;v.f=x;return (v.u&0x7f800000u)!=0x7f800000u;}
float qw_sigmoid(float x){float e=expf(x<0?x:-x);return x<0?e/(1+e):1/(1+e);}
float qw_silu(float x){return x*qw_sigmoid(x);}
void qw_rms(float *o,const float *x,const float *w,unsigned n,int centered){
 float ss=0;for(unsigned i=0;i<n;i++)ss=ss+x[i]*x[i];
 float inv=1/sqrtf(ss/(float)n+1e-6f);
 for(unsigned i=0;i<n;i++)o[i]=(x[i]*inv)*(w[i]+(centered?1.0f:0.0f));
}
void qw_rope(float *x,unsigned dim,unsigned rotary,uint32_t pos,float theta){
 (void)dim;
 for(unsigned i=0;i<rotary/2;i++){
  float freq=powf(theta,-2.0f*(float)i/(float)rotary);
  float angle=(float)pos*freq,c=cosf(angle),s=sinf(angle);
  float a=x[i],b=x[i+rotary/2];x[i]=a*c-b*s;x[i+rotary/2]=b*c+a*s;
 }
}
void qw_delta(float *s,float *q,float *k,const float *v,float decay,float beta,float *o,unsigned d){
 float qn=0,kn=0;
 for(unsigned i=0;i<d;i++){qn=qn+q[i]*q[i];kn=kn+k[i]*k[i];}
 qn=1/sqrtf(qn+1e-6f);kn=1/sqrtf(kn+1e-6f);
 for(unsigned i=0;i<d;i++){q[i]=(q[i]*qn)/sqrtf((float)d);k[i]*=kn;}
 for(unsigned i=0;i<d*d;i++)s[i]*=decay;
 for(unsigned j=0;j<d;j++){
  float mem=0;for(unsigned i=0;i<d;i++)mem=mem+s[i*d+j]*k[i];
  float delta=(v[j]-mem)*beta;
  for(unsigned i=0;i<d;i++)s[i*d+j]=s[i*d+j]+k[i]*delta;
  float sum=0;for(unsigned i=0;i<d;i++)sum=sum+s[i*d+j]*q[i];o[j]=sum;
 }
}
static unsigned u16(const unsigned char*p){return p[0]|((unsigned)p[1]<<8);}
static int32_t i32(const unsigned char*p){uint32_t u=p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);return u<=INT32_MAX?(int32_t)u:-1-(int32_t)(UINT32_MAX-u);}
int qw_reduce(const unsigned char *p,size_t bytes,uint32_t rows,uint32_t groups,
 uint32_t stride,const uint16_t *sw,float sx,float *o){
 if(!p||!sw||!o||!rows||rows>65535||!groups||groups>32||stride<groups*8||stride%32||
  (uint64_t)(rows-1)*stride+(uint64_t)groups*8>bytes||!qw_finite(sx)||sx<=0)return -1;
 for(uint32_t r=0;r<rows;r++)for(uint32_t g=0;g<groups;g++){
  const unsigned char*t=p+(size_t)r*stride+g*8;
  float w=npu_binary16_to_float(sw[(size_t)r*groups+g]);
  if(u16(t)!=r||u16(t+2)!=g||!qw_finite(w)||w<=0)return -1;
 }
 for(uint32_t r=0;r<rows;r++){
  float sum=0;
  for(uint32_t g=0;g<groups;g++){
   const unsigned char*t=p+(size_t)r*stride+g*8;
   /* Contract v1: one FP32 rounding at EACH multiply and ascending addition.
    * Build with -ffp-contract=off, no fast-math/reassociation. */
   float term=(float)i32(t+4)*npu_binary16_to_float(sw[(size_t)r*groups+g]);
   term=term*sx;sum=sum+term;
  }
  if(!qw_finite(sum))return -2;o[r]=sum;
 }
 return 0;
}
