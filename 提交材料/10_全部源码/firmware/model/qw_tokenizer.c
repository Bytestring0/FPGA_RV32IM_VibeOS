#include "qw_tokenizer.h"
#include "qw_runtime.h"
#include "qw_sha256.h"
#include <string.h>
#define NONE UINT32_MAX
static uint32_t r32(const unsigned char*p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t word(const qw_tokenizer*t,uint32_t off){return r32(t->data+off);}
static int within(uint64_t off,uint64_t n,size_t bytes){return off<=bytes&&n<=bytes-off;}
int qt_open(qw_tokenizer*t,const void*data,size_t n,const unsigned char expected[32]){
 (void)expected;
 if(!t||!data||n<128||n>UINT32_MAX)return QW_ARGUMENT;
 const unsigned char*p=data;uint32_t h[24];for(unsigned i=0;i<24;i++)h[i]=r32(p+4*i);
 if(h[0]!=0x31544b51u||h[1]!=1||h[2]!=n||h[3]!=248320||h[4]<256||h[4]>(1u<<21)||(h[4]&(h[4]-1))||h[21]>256)return QW_FORMAT;
 if(!within(h[5],(uint64_t)h[3]*12,n)||!within(h[6],h[7],n)||!within(h[8],(uint64_t)h[4]*16,n)||
 !within(h[9],(uint64_t)h[10]*12,n)||!within(h[11],(uint64_t)h[12]*12,n)||!within(h[13],(uint64_t)h[14]*4,n)||
 !within(h[15],(uint64_t)h[16]*8,n)||!within(h[17],(uint64_t)h[18]*12,n)||!within(h[19],1024,n)||!within(h[20],(uint64_t)h[21]*4,n))return QW_FORMAT;

 t->data=p;t->bytes=n;memcpy(t->h,h,sizeof h);return 0;
}
int qt_piece(const qw_tokenizer*t,uint32_t id,const char**p,size_t*n,unsigned*flags){
 if(!t||!t->data||id>=t->h[3]||!p||!n)return QW_TOKEN;
 uint32_t at=t->h[5]+id*12,off=word(t,at),len=word(t,at+4),f=word(t,at+8);
 if(len==UINT32_MAX||f>2||!within(off,len,t->h[7]))return QW_TOKEN;
 *p=(const char*)t->data+t->h[6]+off;*n=len;if(flags)*flags=f;return 0;
}
static int cp_at(const unsigned char*s,size_t n,size_t*p,uint32_t*out){
 if(*p>=n)return -1;unsigned a=s[(*p)++],extra;uint32_t cp;
 if(a<128){*out=a;return 0;}if(a>=0xc2&&a<=0xdf){extra=1;cp=a&31;}else if(a>=0xe0&&a<=0xef){extra=2;cp=a&15;}else if(a>=0xf0&&a<=0xf4){extra=3;cp=a&7;}else return -1;
 for(unsigned i=0;i<extra;i++){if(*p>=n||(s[*p]&0xc0)!=0x80)return -1;cp=(cp<<6)|(s[(*p)++]&63);}
 if((extra==1&&cp<128)||(extra==2&&cp<2048)||(extra==3&&cp<65536)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return -1;*out=cp;return 0;
}
static uint32_t lookup(const qw_tokenizer*t,uint32_t base,uint32_t count,uint32_t stride,uint32_t cp){
 uint32_t lo=0,hi=count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(word(t,base+m*stride)<cp)lo=m+1;else hi=m;}
 return lo<count&&word(t,base+lo*stride)==cp?lo:NONE;
}
static unsigned combining(const qw_tokenizer*t,uint32_t cp){uint32_t ix=lookup(t,t->h[15],t->h[16],8,cp);return ix==NONE?0:word(t,t->h[15]+ix*8+4);}
static unsigned category(const qw_tokenizer*t,uint32_t cp){
 uint32_t lo=0,hi=t->h[10];while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(word(t,t->h[9]+m*12+4)<cp)lo=m+1;else hi=m;}
 return lo<t->h[10]&&word(t,t->h[9]+lo*12)<=cp?word(t,t->h[9]+lo*12+8):0;
}
static int decompose(const qw_tokenizer*t,uint32_t cp,uint32_t*out,size_t*len,size_t cap,unsigned depth){
 if(depth>32)return QW_FORMAT;
 if(cp>=0xac00&&cp<0xd7a4){uint32_t s=cp-0xac00;if(*len+3>cap)return QW_CAPACITY;out[(*len)++]=0x1100+s/588;out[(*len)++]=0x1161+(s%588)/28;if(s%28)out[(*len)++]=0x11a7+s%28;return 0;}
 uint32_t ix=lookup(t,t->h[11],t->h[12],12,cp);
 if(ix!=NONE){uint32_t at=t->h[11]+ix*12,off=word(t,at+4),n=word(t,at+8);if(!within(off,n,t->h[14]))return QW_FORMAT;
  for(uint32_t i=0;i<n;i++){int rc=decompose(t,word(t,t->h[13]+(off+i)*4),out,len,cap,depth+1);if(rc)return rc;}return 0;
 }
 if(*len==cap)return QW_CAPACITY;out[(*len)++]=cp;return 0;
}
static uint32_t compose(const qw_tokenizer*t,uint32_t a,uint32_t b){
 if(a>=0x1100&&a<0x1113&&b>=0x1161&&b<0x1176)return 0xac00+((a-0x1100)*21+b-0x1161)*28;
 if(a>=0xac00&&a<0xd7a4&&!((a-0xac00)%28)&&b>0x11a7&&b<0x11c3)return a+b-0x11a7;
 uint32_t lo=0,hi=t->h[18];while(lo<hi){uint32_t m=lo+(hi-lo)/2,off=t->h[17]+m*12,x=word(t,off),y=word(t,off+4);if(x<a||(x==a&&y<b))lo=m+1;else hi=m;}
 if(lo<t->h[18]){uint32_t off=t->h[17]+lo*12;if(word(t,off)==a&&word(t,off+4)==b)return word(t,off+8);}return 0;
}
static int normalize(const qw_tokenizer*t,const unsigned char*in,size_t n,uint32_t*cps,size_t ccap,unsigned char*out,size_t cap,size_t*used){
 size_t p=0,len=0;while(p<n){uint32_t cp;if(cp_at(in,n,&p,&cp))return QW_TOKEN;int rc=decompose(t,cp,cps,&len,ccap,0);if(rc)return rc;}
 for(size_t i=1;i<len;i++){unsigned c=combining(t,cps[i]);size_t j=i;while(c&&j&&combining(t,cps[j-1])>c){uint32_t cp=cps[j-1];cps[j-1]=cps[j];cps[j]=cp;j--;}}
 size_t done=0,starter=SIZE_MAX;unsigned prev=0;
 for(size_t i=0;i<len;i++){uint32_t cp=cps[i];unsigned c=combining(t,cp);uint32_t joined=starter!=SIZE_MAX&&(prev<c||prev==0)?compose(t,cps[starter],cp):0;
  if(joined)cps[starter]=joined;else{if(!c)starter=done;cps[done++]=cp;prev=c;}
 }
 size_t bytes=0;for(size_t i=0;i<done;i++){uint32_t cp=cps[i];unsigned size=cp<128?1:cp<2048?2:cp<65536?3:4;if(size>cap-bytes)return QW_CAPACITY;
  if(size==1)out[bytes++]=(unsigned char)cp;
  else{out[bytes++]=(unsigned char)((size==2?0xc0:size==3?0xe0:0xf0)|(cp>>(6*(size-1))));for(unsigned k=size-1;k;k--)out[bytes++]=(unsigned char)(0x80|((cp>>(6*(k-1)))&63));}
 }*used=bytes;return 0;
}
static int pair(const qw_tokenizer*t,uint32_t left,uint32_t right,uint32_t*rank,uint32_t*merged){
 uint32_t pos=((left*0x9e3779b1u)^right)&(t->h[4]-1);
 for(uint32_t i=0;i<t->h[4];i++){uint32_t off=t->h[8]+pos*16,l=word(t,off);if(!l)return 0;
  if(l==left+1&&word(t,off+4)==right+1){*rank=word(t,off+8);*merged=word(t,off+12);return *merged<t->h[3]?1:-1;}
  pos=(pos+1)&(t->h[4]-1);
 }return -1;
}
typedef struct{uint32_t rank,pos,left,right;} node;
static int less(node a,node b){return a.rank<b.rank||(a.rank==b.rank&&a.pos<b.pos);}
static int push(node*heap,size_t*n,size_t cap,node v){if(*n==cap)return QW_CAPACITY;size_t p=(*n)++;while(p){size_t parent=(p-1)/2;if(!less(v,heap[parent]))break;heap[p]=heap[parent];p=parent;}heap[p]=v;return 0;}
static node pop(node*heap,size_t*n){node first=heap[0],v=heap[--*n];size_t p=0;while(p*2+1<*n){size_t c=p*2+1;if(c+1<*n&&less(heap[c+1],heap[c]))c++;if(!less(heap[c],v))break;heap[p]=heap[c];p=c;}if(*n)heap[p]=v;return first;}
static int add_pair(const qw_tokenizer*t,uint32_t pos,uint32_t*ids,uint32_t*next,node*heap,size_t*len,size_t cap){
 if(pos==NONE||next[pos]==NONE)return 0;uint32_t rank,merged;int rc=pair(t,ids[pos],ids[next[pos]],&rank,&merged);if(rc<0)return QW_FORMAT;if(!rc)return 0;return push(heap,len,cap,(node){rank,pos,ids[pos],ids[next[pos]]});
}
static int bpe(const qw_tokenizer*t,const unsigned char*s,size_t n,uint32_t*out,size_t cap,size_t*count,uint32_t*ids,uint32_t*prev,uint32_t*next,node*heap,size_t heap_cap){
 if(!n)return 0;size_t hn=0;
 for(size_t i=0;i<n;i++){ids[i]=word(t,t->h[19]+s[i]*4);prev[i]=i?(uint32_t)i-1:NONE;next[i]=i+1<n?(uint32_t)i+1:NONE;}
 for(uint32_t i=0;i<n;i++){int rc=add_pair(t,i,ids,next,heap,&hn,heap_cap);if(rc)return rc;}
 while(hn){node a=pop(heap,&hn);uint32_t p=a.pos,q=next[p];if(q==NONE||ids[p]!=a.left||ids[q]!=a.right)continue;
  uint32_t rank,merged;int rc=pair(t,ids[p],ids[q],&rank,&merged);if(rc!=1)return QW_FORMAT;
  ids[p]=merged;next[p]=next[q];if(next[q]!=NONE)prev[next[q]]=p;next[q]=NONE;
  if((rc=add_pair(t,prev[p],ids,next,heap,&hn,heap_cap))||(rc=add_pair(t,p,ids,next,heap,&hn,heap_cap)))return rc;
 }
 for(uint32_t p=0;p!=NONE;p=next[p]){if(*count==cap)return QW_CAPACITY;out[(*count)++]=ids[p];}return 0;
}
static uint32_t peek(const unsigned char*s,size_t n,size_t at,size_t*after){uint32_t cp=0;*after=at;if(cp_at(s,n,after,&cp))return NONE;return cp;}
static size_t segment(const qw_tokenizer*t,const unsigned char*s,size_t n,size_t p){
 size_t after;uint32_t cp=peek(s,n,p,&after);unsigned f=category(t,cp);
 if(cp=='\''){static const char*const endings[]={"s","t","re","ve","m","ll","d"};
  for(unsigned i=0;i<7;i++){size_t q=after;const char*e=endings[i];while(*e&&q<n){size_t end;uint32_t c=peek(s,n,q,&end);if(c>='A'&&c<='Z')c+=32;if(c==0x17f)c='s';if(c!=(unsigned char)*e)break;q=end;e++;}if(!*e)return q;}
 }
 size_t a=after;unsigned nf=0;if(a<n){size_t end;nf=category(t,peek(s,n,a,&end));}
 if(cp!='\r'&&cp!='\n'&&!(f&(1|4))&&(nf&(1|2))){p=after;f=nf;}
 if(f&(1|2)){size_t q=p;while(q<n){size_t e;unsigned c=category(t,peek(s,n,q,&e));if(!(c&(1|2)))break;q=e;}return q;}
 if(f&4)return after;
 size_t q=p;if(cp==' '&&after<n)q=after;
 size_t start=q;
 while(q<n){size_t e;uint32_t c=peek(s,n,q,&e);if(category(t,c)&(1|2|4|8))break;q=e;}
 if(q>start){while(q<n){size_t e;uint32_t c=peek(s,n,q,&e);if(c!='\r'&&c!='\n')break;q=e;}return q;}
 q=p;size_t last_newline=p,last_space=p;unsigned spaces=0;
 while(q<n){size_t e;uint32_t c=peek(s,n,q,&e);if(!(category(t,c)&8))break;last_space=q;spaces++;q=e;if(c=='\r'||c=='\n')last_newline=q;}
 if(last_newline>p)return last_newline;if(spaces)return q<n&&spaces>1?last_space:q;
 return after;
}
size_t qt_workspace_bytes(size_t n){if(n>32768)return 0;size_t cells=n*4+8;return (n*2+8)*4+cells+cells*12+cells*3*sizeof(node);}
int qt_encode(const qw_tokenizer*t,const char*text,size_t n,int special,uint32_t*out,size_t cap,size_t*count,void*workspace,size_t bytes){
 if(count)*count=0;if(!t||!t->data||!text||!out||!count||n>32768)return QW_ARGUMENT;if(!n)return 0;
 size_t need=qt_workspace_bytes(n);if(!workspace||((uintptr_t)workspace&3)||bytes<need)return QW_CAPACITY;
 size_t cells=n*4+8;unsigned char*w=workspace;uint32_t*cps=(uint32_t*)w;w+=(n*2+8)*4;unsigned char*normalized=w;w+=cells;
 uint32_t*ids=(uint32_t*)w;w+=cells*4;uint32_t*prev=(uint32_t*)w;w+=cells*4;uint32_t*next=(uint32_t*)w;w+=cells*4;node*heap=(node*)w;
 size_t len=0;int rc=normalize(t,(const unsigned char*)text,n,cps,n*2+8,normalized,cells,&len);if(rc)return rc;
 size_t p=0;
 while(p<len){size_t marker=len;uint32_t added=NONE;size_t marker_len=0;
  for(size_t q=p;q<len;q++){for(uint32_t k=0;k<t->h[21];k++){
    uint32_t id=word(t,t->h[20]+k*4);const char*piece;size_t pn;unsigned flags;
    if(qt_piece(t,id,&piece,&pn,&flags))return QW_FORMAT;if(!special&&flags==1)continue;
    if(pn&&pn<=len-q&&!memcmp(normalized+q,piece,pn)&&pn>marker_len){marker=q;added=id;marker_len=pn;}
   }if(added!=NONE)break;}
  while(p<marker){size_t end=segment(t,normalized,marker,p);if(end<=p||end>marker)return QW_FORMAT;
   if((rc=bpe(t,normalized+p,end-p,out,cap,count,ids,prev,next,heap,cells*3)))return rc;p=end;
  }
  if(added!=NONE){if(*count==cap)return QW_CAPACITY;out[(*count)++]=added;p+=marker_len;}
 }
 return 0;
}
