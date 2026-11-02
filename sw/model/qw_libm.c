#include "qw_libm.h"
#include <stdint.h>
static float bits(uint32_t u){union{uint32_t u;float f;}v;v.u=u;return v.f;}
float qwm_exp(float x){
 if(x!=x)return x;if(x>88.722839f)return bits(0x7f800000);if(x<-103.972084f)return 0;
 float scaled=x*1.4426950408889634f;int n=(int)(scaled+(scaled>=0?0.5f:-0.5f));
 float r=(x-(float)n*0.693145751953125f)-(float)n*1.428606765330187e-6f;
 float p=1.0f+r*(1.0f+r*(0.5f+r*(0.16666666666666667f+r*(0.041666666666666664f+r*(0.008333333333333333f+r*(0.001388888888888889f+r*(0.0001984126984126984f+r*0.0000248015873015873f)))))));
 if(n==128)return(p*bits(254u<<23))*2.0f;
 if(n<-126)return(p*bits((uint32_t)(n+151)<<23))*0x1p-24f;
 return p*bits((uint32_t)(n+127)<<23);
}
static float log_positive(float x){
 union{uint32_t u;float f;}v;v.f=x;int e=0;
 if((v.u&0x7f800000u)==0){x*=0x1p24f;e=-24;v.f=x;}
 e+=(int)((v.u>>23)&255)-127;v.u=(v.u&0x7fffffu)|0x3f800000u;
 float z=(v.f-1)/(v.f+1),z2=z*z,p=z,sum=z;
 for(unsigned i=3;i<=23;i+=2){p*=z2;sum+=p/(float)i;}
 return(float)e*0.6931471805599453f+2*sum;
}
float qwm_log1p(float x){
 if(x!=x)return x;if(x==-1)return-bits(0x7f800000);if(x<-1)return bits(0x7fc00000);
 if(x==bits(0x7f800000))return x;
 if(x>-0.0625f&&x<0.0625f){float p=x,sum=x;for(unsigned i=2;i<=10;i++){p*=-x;sum+=p/(float)i;}return sum;}
 float u=1+x;return log_positive(u)+(x-(u-1))/u;
}
float qwm_sqrt(float x){
 union{uint32_t u;float f;}v;v.f=x;
 if(x==0||x!=x||x==bits(0x7f800000))return x;
 if(x<0)return bits(0x7fc00000);float scale=1;
 if(v.u<0x00800000u){x*=0x1p24f;scale=0x1p-12f;v.f=x;}
 v.u=(v.u>>1)+0x1fc00000u;float y=v.f;
 for(unsigned i=0;i<6;i++)y=0.5f*(y+x/y);return y*scale;
}
static float trig(float x,int cosine){
 if(x!=x||x>1048576||x<-1048576)return bits(0x7fc00000);
 double scaled=(double)x*0.63661977236758134308;int n=(int)(scaled+(scaled>=0?0.5:-0.5));
 float r=(float)((double)x-(double)n*1.57079632679489661923),s=r*r;
 float sn=r+r*s*(-0.16666666666666667f+s*(0.008333333333333333f+s*(-0.0001984126984126984f+s*(0.0000027557319223985893f-s*0.00000002505210838544172f))));
 float cs=1+s*(-0.5f+s*(0.041666666666666664f+s*(-0.001388888888888889f+s*(0.0000248015873015873f-s*0.0000002755731922398589f))));
 unsigned q=((unsigned)n+(cosine?1u:0u))&3u;return q==0?sn:q==1?cs:q==2?-sn:-cs;
}
float qwm_sin(float x){return trig(x,0);}
float qwm_cos(float x){return trig(x,1);}
float qwm_pow(float x,float y){if(x<=0)return x==0&&y>0?0:bits(0x7fc00000);return qwm_exp(log_positive(x)*y);}
