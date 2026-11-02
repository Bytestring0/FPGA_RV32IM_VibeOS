#include "npu_tensor_v2.h"
#include <limits.h>

static int native_overlap(const void *a,size_t an,const void *b,size_t bn)
{
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 if(an==0||bn==0)return 0;
 if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return 1;
 return x<y+bn&&y<x+an;
}
static void span(npu_tensor_layout *l,uint32_t o,uint32_t words,uint32_t access)
{
 if(words!=0){npu_tensor_span *s=&l->spans[l->span_count++];s->offset=o;s->bytes=words*4;s->access=access;}
}
uint32_t npu_tensor_query(const npu_tensor_job *job,npu_tensor_layout *layout)
{
 npu_tensor_job j; npu_tensor_layout l={{{0,0,0}},0,98304};
 uint32_t na,nb=0,nc=0,no,ns=0,i,t;uint64_t raw_bytes=0;
 if(!job||!layout)return NPU_TENSOR_ERR_ARGUMENT;
 if(native_overlap(job,sizeof(*job),layout,sizeof(*layout)))return NPU_TENSOR_ERR_OVERLAP;
 j=*job;
 if(j.opcode<1||j.opcode>20)return NPU_TENSOR_ERR_OPCODE;
 if(j.n==0||j.n>4096)return NPU_TENSOR_ERR_DIMENSION;
 na=no=j.n;
 switch(j.opcode){
 case 2:case 3:case 8:nb=j.n;break;
 case 10:if(j.n&1)return NPU_TENSOR_ERR_DIMENSION;nb=nc=j.n/2;break;
 case 11:if(!j.m||j.m>32)return NPU_TENSOR_ERR_DIMENSION;na=nb=j.n*j.m;nc=j.param0?j.n:0;break;
 case 12:if(j.n>1024||!j.k||j.k>8)return NPU_TENSOR_ERR_DIMENSION;nb=j.n*j.k;ns=j.n*(j.k-1);break;
 case 13:case 20:if(j.n>128)return NPU_TENSOR_ERR_DIMENSION;nb=nc=j.n;ns=j.n*j.n;break;
 case 14:if(j.n>32||!j.k||j.k>256)return NPU_TENSOR_ERR_DIMENSION;na=no=j.k;nb=nc=j.n*j.k;break;
 case 15:no=0;break;
 case 16:no=1;break;
 case 17:if(j.n>32||!j.k||j.k>4096)return NPU_TENSOR_ERR_DIMENSION;na=j.n*j.k;nb=j.k;break;
 case 18:if(j.n>32||!j.k||j.k>256)return NPU_TENSOR_ERR_DIMENSION;nb=j.n*j.k;no=j.k;break;
 case 19:if(!j.m||j.m>32)return NPU_TENSOR_ERR_DIMENSION;na=0;raw_bytes=(uint64_t)(j.n-1)*j.stride_a+(uint64_t)j.m*8;break;
 default:break;
 }
 if((j.opcode==8&&j.param1>1)||(j.opcode==11&&j.param0>1)||
    ((j.opcode==13||j.opcode==20)&&(j.param0>1073741824u||j.param1>1073741824u))||
    (j.opcode==14&&j.param0>INT32_MAX)||
    (j.opcode==19&&(j.param0>UINT32_C(0x80000000)||j.c||j.param1||j.param2)))return NPU_TENSOR_ERR_PARAMETER;
 if((j.opcode==19?(j.stride_a<j.m*8||(j.stride_a&31)):j.stride_a)||j.stride_b||j.stride_out||j.flags)return NPU_TENSOR_ERR_STRIDE;
 if(j.opcode==19){
  l.spans[l.span_count++]=(npu_tensor_span){j.a,raw_bytes>UINT32_MAX?UINT32_MAX:(uint32_t)raw_bytes,1};
  l.spans[l.span_count++]=(npu_tensor_span){j.b,j.n*j.m*2,1};
 }else {span(&l,j.a,na,1);span(&l,j.b,nb,1);span(&l,j.c,nc,1);}
 span(&l,j.out,no,2);span(&l,j.state,ns,3);
 if(j.opcode==15){npu_tensor_span *s=&l.spans[l.span_count++];s->offset=j.out;s->bytes=j.n;s->access=2;}
 for(i=0;i<l.span_count;i++)if(l.spans[i].offset&31)return NPU_TENSOR_ERR_ALIGNMENT;
 for(i=0;i<l.span_count;i++)if((uint64_t)l.spans[i].offset+l.spans[i].bytes>NPU_TENSOR_DDR_LIMIT)return NPU_TENSOR_ERR_RANGE;
 for(i=0;i<l.span_count;i++)for(t=0;t<i;t++)if(((l.spans[i].access|l.spans[t].access)&2)&&
  (uint64_t)l.spans[i].offset< (uint64_t)l.spans[t].offset+l.spans[t].bytes&&
  (uint64_t)l.spans[t].offset< (uint64_t)l.spans[i].offset+l.spans[i].bytes)return NPU_TENSOR_ERR_OVERLAP;
 *layout=l;return 0;
}
static void flag(npu_fixed_status *s,uint32_t f)
{
 uint32_t *p=0;s->status|=f;
 if(f==2)p=&s->saturations;else if(f==4)p=&s->underflows;
 else if(f==1)p=&s->domain_clamps;else if(f==32)p=&s->zero_vectors;
 if(p&&*p!=UINT32_MAX)++*p;
}
static uint64_t mag(int64_t x){return x<0?(uint64_t)(-(x+1))+1:(uint64_t)x;}
static int64_t signmag(uint64_t u,int neg){if(neg&&u==(UINT64_C(1)<<63))return INT64_MIN;return neg?-(int64_t)u:(int64_t)u;}
static int64_t divide(int64_t x,uint64_t d,npu_fixed_status *s)
{
 uint64_t u=mag(x),q=u/d,r=u%d;
 if(r>d-r||(r==d-r&&(q&1)))++q;
 if(x&&q==0)flag(s,4);
 return signmag(q,x<0);
}
static int64_t add(int64_t a,int64_t b,npu_fixed_status *s)
{
 if(b>0&&a>INT64_MAX-b){flag(s,2);return INT64_MAX;}
 if(b<0&&a<INT64_MIN-b){flag(s,2);return INT64_MIN;}return a+b;
}
static int32_t narrow(int64_t a,npu_fixed_status *s)
{if(a>INT32_MAX){flag(s,2);return INT32_MAX;}if(a<INT32_MIN){flag(s,2);return INT32_MIN;}return (int32_t)a;}
/* Exact signed 128-bit product represented by two uint64 limbs. This avoids
 * compiler __int128 extensions unavailable in the RV32 compiler. */
static int64_t mulshift(int64_t a,int64_t b,unsigned sh,npu_fixed_status *s)
{
 uint64_t x=mag(a),y=mag(b),x0=(uint32_t)x,y0=(uint32_t)y,x1=x>>32,y1=y>>32;
 uint64_t z=x0*y0,w0=(uint32_t)z,t=x1*y0+(z>>32),w1=(uint32_t)t,w2=t>>32;
 uint64_t hi,lo,q,r,limit;int neg=(a<0)!=(b<0);
 t=x0*y1+w1;hi=x1*y1+w2+(t>>32);lo=(t<<32)+w0;
 q=(lo>>sh)|(hi<<(64-sh));r=lo&((UINT64_C(1)<<sh)-1);
 limit=neg?(UINT64_C(1)<<63):(uint64_t)INT64_MAX;
 if((hi>>sh)||q>limit){flag(s,2);return neg?INT64_MIN:INT64_MAX;}
 if(r>(UINT64_C(1)<<(sh-1))||(r==(UINT64_C(1)<<(sh-1))&&(q&1))){
  if(q==limit){flag(s,2);return neg?INT64_MIN:INT64_MAX;}++q;
 }
 if(x&&y&&!q)flag(s,4);
 return signmag(q,neg);
}
static uint32_t loadu(const npu_tensor_mapping *m,uint32_t off)
{
 const uint8_t *p=m->data+(size_t)(off-m->ddr_base);
 return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int32_t load(const npu_tensor_mapping *m,uint32_t base,uint32_t i)
{uint32_t u=loadu(m,base+4*i);return u<=INT32_MAX?(int32_t)u:-1-(int32_t)(UINT32_MAX-u);}
static uint16_t load16(const npu_tensor_mapping *m,uint32_t off)
{const uint8_t *p=m->data+(size_t)(off-m->ddr_base);return (uint16_t)((uint16_t)p[0]|(uint16_t)p[1]<<8);}
static int32_t raw_coefficient(uint16_t bits,uint32_t maximum,npu_fixed_status *s)
{
 unsigned e=(bits>>10)&31,mant=bits&1023;int sh=e?(int)e-11:-10;
 uint64_t numerator,denominator=127;
 if(e)mant|=1024;
 numerator=(uint64_t)mant*maximum;
 if(sh>=0)numerator<<=(unsigned)sh;else denominator<<=(unsigned)-sh;
 return narrow(divide((int64_t)numerator,denominator,s),s);
}
static void store(const npu_tensor_mapping *m,uint32_t base,uint32_t i,int32_t x)
{uint8_t *p=m->data+(size_t)(base+4*i-m->ddr_base);unsigned k;for(k=0;k<4;k++)p[k]=(uint8_t)((uint32_t)x>>(8*k));}
static uint64_t sqrt_floor(uint64_t a)
{
 uint64_t r=0,b=UINT64_C(1)<<62;
 while(b>a)b>>=2;
 while(b){if(a>=r+b){a-=r+b;r=(r>>1)+b;}else r>>=1;b>>=2;}return r;
}
static uint32_t expq(int64_t x,npu_fixed_status *s)
{
 int64_t q=divide(x,32,s);
 if(q<-32768){q=-32768;flag(s,1);}if(q>0){q=0;flag(s,1);}
 return npu_fixed_exp_negative_q30((int32_t)q,NPU_FIXED_LUT_LINEAR,s);
}
static void softmax(int32_t *out,int32_t *v,uint32_t n,npu_fixed_status *s)
{
 uint32_t i;int32_t mx=v[0];uint64_t total=0;
 for(i=1;i<n;i++)if(v[i]>mx)mx=v[i];
 for(i=0;i<n;i++){out[i]=(int32_t)expq((int64_t)v[i]-mx,s);total+=(uint32_t)out[i];}
 for(i=0;i<n;i++)out[i]=(int32_t)divide((int64_t)out[i]*65536,total,s);
}
static void compute(const npu_tensor_job *j,const npu_tensor_mapping *m,npu_tensor_stats *st)
{
 int32_t out[4096],tmp[4096],state[16384];
 npu_fixed_status *s=&st->numeric;uint32_t i,t,z,no=j->n,ns=0;
 int64_t acc,term;uint64_t peak=0;
 switch(j->opcode){
 case 1:for(i=0;i<j->n;i++)out[i]=load(m,j->a,i);break;
 case 2:for(i=0;i<j->n;i++)out[i]=narrow((int64_t)load(m,j->a,i)+load(m,j->b,i),s);break;
 case 3:for(i=0;i<j->n;i++)out[i]=narrow(mulshift(load(m,j->a,i),load(m,j->b,i),16,s),s);break;
 case 4:case 5:case 6:case 7:
  for(i=0;i<j->n;i++){int32_t a=load(m,j->a,i);
   if(j->opcode==4)out[i]=npu_fixed_sigmoid_q16(a,NPU_FIXED_LUT_LINEAR,s);
   else if(j->opcode==5)out[i]=npu_fixed_silu_q16(a,NPU_FIXED_LUT_LINEAR,s);
   else if(j->opcode==6)out[i]=npu_fixed_softplus_q16(a,NPU_FIXED_LUT_LINEAR,s);
   else out[i]=(int32_t)divide(expq(a,s),16384,s);
  }break;
 case 8:{
  uint64_t mean=0,rem=0,eps=j->param0?j->param0:4295,root;
  for(i=0;i<j->n;i++){uint64_t a=mag(load(m,j->a,i)),sq=a*a;mean+=sq/j->n;rem+=sq%j->n;}
  mean+=rem/j->n;rem%=j->n;if(rem>j->n-rem||(rem==j->n-rem&&(mean&1)))++mean;
  if(UINT64_MAX-mean<eps){mean=UINT64_MAX;flag(s,2);}else mean+=eps;
  root=sqrt_floor(mean); /* epsilon>=1 guarantees nonzero denominator. */
  for(i=0;i<j->n;i++){
   int64_t norm=divide((int64_t)load(m,j->a,i)*65536,root,s);
   int64_t weight=(int64_t)load(m,j->b,i)+(j->param1?65536:0);
   out[i]=narrow(mulshift(norm,weight,16,s),s);
  }break;
 }
 case 9:for(i=0;i<j->n;i++)tmp[i]=load(m,j->a,i);softmax(out,tmp,j->n,s);break;
 case 10:
  for(i=0;i<j->n/2;i++){
   int32_t a=load(m,j->a,i),b=load(m,j->a,i+j->n/2),c=load(m,j->b,i),d=load(m,j->c,i);
   acc=add((int64_t)a*c,-(int64_t)b*d,s);out[i]=narrow(divide(acc,UINT64_C(1)<<30,s),s);
   acc=add((int64_t)b*c,(int64_t)a*d,s);out[i+j->n/2]=narrow(divide(acc,UINT64_C(1)<<30,s),s);
  }break;
 case 11:
  for(i=0;i<j->n;i++){acc=0;for(t=0;t<j->m;t++){z=i*j->m+t;term=mulshift(load(m,j->a,z),load(m,j->b,z),14,s);acc=add(acc,term,s);}if(j->param0)acc=add(acc,load(m,j->c,i),s);out[i]=narrow(acc,s);}break;
 case 12:
  ns=j->n*(j->k-1);
  for(i=0;i<j->n;i++){
   acc=0;for(t=0;t<j->k;t++){
    int32_t v=t+1==j->k?load(m,j->a,i):load(m,j->state,i*(j->k-1)+t);
    acc=add(acc,(int64_t)v*load(m,j->b,i*j->k+t),s);
    if(t+1<j->k)state[i*(j->k-1)+t]=t+2==j->k?load(m,j->a,i):load(m,j->state,i*(j->k-1)+t+1);
   }out[i]=narrow(divide(acc,65536,s),s);
  }break;
 case 13:case 20:
  ns=j->n*j->n;
  for(i=0;i<ns;i++)state[i]=narrow(mulshift(load(m,j->state,i),j->param0,30,s),s);
  for(t=0;t<j->n;t++){
   acc=0;for(i=0;i<j->n;i++)acc=add(acc,(int64_t)state[i*j->n+t]*load(m,j->b,i),s);
   term=divide(acc,UINT64_C(1)<<24,s); /* mem is int32 Q16 at this stage. */
   tmp[t]=narrow(term,s);
   tmp[t]=narrow(mulshift((int64_t)load(m,j->c,t)-tmp[t],j->param1,30,s),s);
  }
  for(i=0;i<j->n;i++)for(t=0;t<j->n;t++)state[i*j->n+t]=narrow(add(state[i*j->n+t],mulshift(load(m,j->b,i),tmp[t],8,s),s),s);
  for(t=0;t<j->n;t++){acc=0;for(i=0;i<j->n;i++)acc=add(acc,(int64_t)state[i*j->n+t]*load(m,j->a,i),s);out[t]=narrow(divide(acc,UINT64_C(1)<<(j->opcode==20?16:24),s),s);}break;
 case 14:
  no=j->k;
  for(i=0;i<j->n;i++){acc=0;for(t=0;t<j->k;t++)acc=add(acc,(int64_t)load(m,j->a,t)*load(m,j->b,i*j->k+t),s);tmp[i]=narrow(mulshift(acc,j->param0,46,s),s);}
  softmax(out,tmp,j->n,s);for(i=0;i<j->n;i++)tmp[i]=out[i];
  for(t=0;t<j->k;t++){acc=0;for(i=0;i<j->n;i++)acc=add(acc,(int64_t)tmp[i]*load(m,j->c,i*j->k+t),s);out[t]=narrow(divide(acc,65536,s),s);}break;
 case 15:
  for(i=0;i<j->n;i++){uint64_t v=mag(load(m,j->a,i));if(v>peak)peak=v;}st->result=(uint32_t)peak;
  if(!peak)flag(s,32);
  for(i=0;i<j->n;i++)out[i]=peak?(int32_t)divide((int64_t)load(m,j->a,i)*127,peak,s):0;
  break;
 case 16:{int32_t mx=load(m,j->a,0);uint32_t idx=0;no=1;for(i=1;i<j->n;i++){int32_t a=load(m,j->a,i);if(a>mx){mx=a;idx=i;}}out[0]=(int32_t)idx;st->result=idx;break;}
 case 17:
  for(i=0;i<j->n;i++){acc=0;for(t=0;t<j->k;t++)acc=add(acc,(int64_t)load(m,j->a,i*j->k+t)*load(m,j->b,t),s);out[i]=narrow(divide(acc,65536,s),s);}break;
 case 18:
  no=j->k;for(t=0;t<j->k;t++){acc=0;for(i=0;i<j->n;i++)acc=add(acc,(int64_t)load(m,j->a,i)*load(m,j->b,i*j->k+t),s);out[t]=narrow(divide(acc,65536,s),s);}break;
 case 19:
  for(i=0;i<j->n;i++){
   acc=0;for(t=0;t<j->m;t++){
    int32_t partial=load(m,j->a+i*j->stride_a+t*8+4,0);
    int32_t coef=raw_coefficient(load16(m,j->b+(i*j->m+t)*2),j->param0,s);
    acc=add(acc,mulshift(partial,coef,14,s),s);
   }out[i]=narrow(acc,s);
  }break;
 default:break;
 }
 if(j->opcode==15)for(i=0;i<no;i++)m->data[(size_t)(j->out-m->ddr_base)+i]=(uint8_t)out[i];
 else for(i=0;i<no;i++)store(m,j->out,i,out[i]);
 for(i=0;i<ns;i++)store(m,j->state,i,state[i]);
}
uint32_t npu_tensor_reference(const npu_tensor_job *job,const npu_tensor_mapping *mapping,npu_tensor_stats *stats)
{
 npu_tensor_job j;npu_tensor_mapping m;npu_tensor_layout l;npu_tensor_stats st={0,0,{0,0,0,0,0,0}};
 uint32_t e,i,t;
 if(!job||!mapping)return NPU_TENSOR_ERR_ARGUMENT;
 j=*job;m=*mapping;
 /* Host descriptor/mapping may live in the mapped DDR; snapshot them first.
  * Stats and descriptor/mapping must never be overwritten through DDR. */
 if(stats&&(native_overlap(stats,sizeof(*stats),job,sizeof(*job))||native_overlap(stats,sizeof(*stats),mapping,sizeof(*mapping))||native_overlap(stats,sizeof(*stats),m.data,m.capacity)))return NPU_TENSOR_ERR_OVERLAP;
 e=npu_tensor_query(&j,&l);
 if(!e&&(!m.data||m.ddr_base>NPU_TENSOR_DDR_LIMIT||m.capacity>NPU_TENSOR_DDR_LIMIT-m.ddr_base||m.capacity>UINTPTR_MAX-(uintptr_t)m.data))e=NPU_TENSOR_ERR_MAPPING;
 if(!e)for(i=0;i<l.span_count;i++){
  npu_tensor_span *s=&l.spans[i];
  if(s->offset<m.ddr_base||(uint64_t)s->offset+s->bytes>(uint64_t)m.ddr_base+m.capacity){e=NPU_TENSOR_ERR_MAPPING;break;}
  if(s->access&2){const void *p=m.data+(size_t)(s->offset-m.ddr_base);
   if(native_overlap(p,s->bytes,job,sizeof(*job))||native_overlap(p,s->bytes,mapping,sizeof(*mapping))){e=NPU_TENSOR_ERR_OVERLAP;break;}
  }
 }
 if(!e&&j.opcode==19)for(i=0;i<j.n&&!e;i++)for(t=0;t<j.m;t++){
  uint16_t bits;
  if(loadu(&m,j.a+i*j.stride_a+t*8)!=(i|(t<<16))){e=NPU_TENSOR_ERR_TAG;break;}
  bits=load16(&m,j.b+(i*j.m+t)*2);
  if((bits&0x8000)||((bits>>10)&31)==31||!(bits&0x7fff)){
   e=NPU_TENSOR_ERR_PARAMETER;st.numeric.status=NPU_FIXED_STATUS_INVALID_ARGUMENT;st.numeric.invalid_inputs=1;break;
  }
 }
 st.error=e;if(!e)compute(&j,&m,&st);if(stats)*stats=st;return e;
}
