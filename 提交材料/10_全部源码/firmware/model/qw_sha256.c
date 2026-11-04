#include "qw_sha256.h"
#include <string.h>
static uint32_t rotr(uint32_t x,unsigned n){return(x>>n)|(x<<(32-n));}
static void block(qw_sha256*s,const unsigned char*p){
 static const uint32_t k[64]={
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
 uint32_t w[64];
 typedef uint32_t sha_word __attribute__((__may_alias__));
 if(!((uintptr_t)p&3u)){
  const sha_word *words=(const sha_word *)p;
  for(unsigned i=0;i<16;i++){
   uint32_t x=words[i];
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
   x=((x&0xffu)<<24)|((x&0xff00u)<<8)|((x>>8)&0xff00u)|(x>>24);
#endif
   w[i]=x;
  }
 }else for(unsigned i=0;i<16;i++)w[i]=((uint32_t)p[4*i]<<24)|((uint32_t)p[4*i+1]<<16)|((uint32_t)p[4*i+2]<<8)|p[4*i+3];
 for(unsigned i=16;i<64;i++){uint32_t a=w[i-15],b=w[i-2];w[i]=w[i-16]+(rotr(a,7)^rotr(a,18)^(a>>3))+w[i-7]+(rotr(b,17)^rotr(b,19)^(b>>10));}
 uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
 for(unsigned i=0;i<64;i++){uint32_t t=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];uint32_t u=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
 s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
void qw_sha_init(qw_sha256*s){static const uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(s->h,h,sizeof h);s->used=0;s->bytes=0;}
void qw_sha_update(qw_sha256*s,const void*data,size_t n){
 const unsigned char*p=data;s->bytes+=n;
 if(s->used){size_t m=64u-s->used;if(m>n)m=n;memcpy(s->block+s->used,p,m);s->used+=(unsigned)m;p+=m;n-=m;if(s->used==64u){block(s,s->block);s->used=0;}}
 while(n>=64u){block(s,p);p+=64;n-=64;}
 if(n){memcpy(s->block,p,n);s->used=(unsigned)n;}
}
void qw_sha_final(qw_sha256*s,unsigned char out[32]){uint64_t bits=s->bytes*8;s->block[s->used++]=128;if(s->used>56){memset(s->block+s->used,0,64-s->used);block(s,s->block);s->used=0;}memset(s->block+s->used,0,56-s->used);for(unsigned i=0;i<8;i++)s->block[63-i]=(unsigned char)(bits>>(8*i));block(s,s->block);for(unsigned i=0;i<32;i++)out[i]=(unsigned char)(s->h[i/4]>>(24-8*(i%4)));}
