#include "qw_lut_ops.h"
#include <string.h>

#if NPU_FIXED_NUMERIC_VERSION != UINT32_C(0x00010000) || \
    NPU_FIXED_Q16_LUT_NUMERIC_VERSION != UINT32_C(0x00010001)
#error "LUT adapters require the declared base and Q16-LUT numeric versions"
#endif
/* sizeof(float)==4 is required only for the bit-copy ABI, never arithmetic. */
_Static_assert(sizeof(float)==sizeof(uint32_t),"binary32 storage required");
static uint32_t add32(uint32_t a,uint32_t b){return b>UINT32_MAX-a?UINT32_MAX:a+b;}
static uint64_t increment64(uint64_t a){return a==UINT64_MAX?a:a+1;}
static void note(npu_fixed_status*s,uint32_t flags){
 if(!s)return;s->status|=flags;
 if(flags&NPU_FIXED_STATUS_DOMAIN_CLAMP)s->domain_clamps=add32(s->domain_clamps,1);
 if(flags&NPU_FIXED_STATUS_UNDERFLOW)s->underflows=add32(s->underflows,1);
}
static void merge(npu_fixed_status*d,const npu_fixed_status*s){
 if(!d)return;d->status|=s->status;
 d->domain_clamps=add32(d->domain_clamps,s->domain_clamps);
 d->saturations=add32(d->saturations,s->saturations);
 d->underflows=add32(d->underflows,s->underflows);
 d->invalid_inputs=add32(d->invalid_inputs,s->invalid_inputs);
 d->zero_vectors=add32(d->zero_vectors,s->zero_vectors);
}
/* Magnitude rounding avoids implementation-defined negative right shifts. */
static uint32_t right_rne(uint32_t mag,unsigned shift){
 uint32_t q=mag>>shift,tail=mag&((UINT32_C(1)<<shift)-1u);
 uint32_t half=UINT32_C(1)<<(shift-1u);
 return q+(uint32_t)(tail>half||(tail==half&&(q&1u)));
}
static int32_t input_fixed(uint32_t bits,unsigned shift,npu_fixed_status*s){
 int32_t q16=npu_fixed_f32_bits_to_q16(bits,s);
 uint32_t mag=q16<0?UINT32_C(0)-(uint32_t)q16:(uint32_t)q16;
 uint32_t q=right_rne(mag,shift);
 if(mag&&!q)note(s,NPU_FIXED_STATUS_UNDERFLOW);
 return q16<0?-(int32_t)q:(int32_t)q;
}
/* int32 Q16 and unsigned Q30 results are zero or normal binary32 numbers.
 * The highest bit is <=31. No subnormal/overflow case is possible here. */
static uint32_t pack_fixed(uint32_t mag,uint32_t sign,unsigned fraction_bits){
 if(!mag)return 0;
 unsigned highest=0;uint32_t scan=mag;
 while(scan>>1){scan>>=1;highest++;}
 uint32_t mantissa=highest<=23?mag<<(23-highest):right_rne(mag,highest-23);
 if(mantissa==UINT32_C(0x1000000)){mantissa>>=1;highest++;}
 return sign|((highest+127u-fraction_bits)<<23)|(mantissa&UINT32_C(0x7fffff));
}
static uint32_t pack_q16(int32_t q){
 return pack_fixed(q<0?UINT32_C(0)-(uint32_t)q:(uint32_t)q,
                   q<0?UINT32_C(0x80000000):0,16);
}
typedef int32_t (*q16_op)(int32_t,npu_fixed_lut_mode,npu_fixed_status*);
static uint32_t apply_q16(uint32_t bits,npu_fixed_status*s,q16_op op){
 int32_t x=npu_fixed_f32_bits_to_q16(bits,s);
 /* Report an actual out-of-core input even when quantization rounds it onto
  * the endpoint; the primitive reports all other finite tail extensions. */
 uint32_t mag=bits&UINT32_C(0x7fffffff);
 if(mag>UINT32_C(0x41800000)&&mag<UINT32_C(0x7f800000)&&x>=-1048576&&x<=1048576)
  note(s,NPU_FIXED_STATUS_DOMAIN_CLAMP);
 return pack_q16(op(x,NPU_FIXED_LUT_LINEAR,s));
}
uint32_t qw_lut_sigmoid_bits(uint32_t x,npu_fixed_status*s){return apply_q16(x,s,npu_fixed_sigmoid_q16);}
uint32_t qw_lut_silu_bits(uint32_t x,npu_fixed_status*s){return apply_q16(x,s,npu_fixed_silu_q16);}
uint32_t qw_lut_softplus_bits(uint32_t x,npu_fixed_status*s){return apply_q16(x,s,npu_fixed_softplus_q16);}
uint32_t qw_lut_exp_negative_bits(uint32_t bits,npu_fixed_status*s){
 int32_t x=input_fixed(bits,5,s);uint32_t mag=bits&UINT32_C(0x7fffffff);
 if(mag<UINT32_C(0x7f800000)&&
    (((bits>>31)&&mag>UINT32_C(0x41800000)&&x>=-32768)||
     (!(bits>>31)&&mag&&x<=0)))note(s,NPU_FIXED_STATUS_DOMAIN_CLAMP);
 return pack_fixed(npu_fixed_exp_negative_q30(x,NPU_FIXED_LUT_LINEAR,s),0,30);
}
typedef uint32_t (*bits_op)(uint32_t,npu_fixed_status*);
static int adapt(float input,float*output,qw_lut_status*s,bits_op op){
 if(!output||!s)return QW_ARGUMENT;
 uint32_t in_bits,out_bits;npu_fixed_status local={0};
 memcpy(&in_bits,&input,sizeof in_bits);out_bits=op(in_bits,&local);
 merge(&s->fixed,&local);
 if(local.status&(NPU_FIXED_STATUS_ERROR_MASK|NPU_FIXED_STATUS_SATURATED))return QW_NUMERIC;
 memcpy(output,&out_bits,sizeof out_bits);return 0;
}
int qw_lut_sigmoid(float x,float*y,qw_lut_status*s){
 if(s)s->sigmoid_calls=increment64(s->sigmoid_calls);return adapt(x,y,s,qw_lut_sigmoid_bits);
}
int qw_lut_silu(float x,float*y,qw_lut_status*s){
 if(s)s->silu_calls=increment64(s->silu_calls);return adapt(x,y,s,qw_lut_silu_bits);
}
int qw_lut_softplus(float x,float*y,qw_lut_status*s){
 if(s)s->softplus_calls=increment64(s->softplus_calls);return adapt(x,y,s,qw_lut_softplus_bits);
}
int qw_lut_exp_negative(float x,float*y,qw_lut_status*s){
 if(s)s->exp_negative_calls=increment64(s->exp_negative_calls);return adapt(x,y,s,qw_lut_exp_negative_bits);
}
