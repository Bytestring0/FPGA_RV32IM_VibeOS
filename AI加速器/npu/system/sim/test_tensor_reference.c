#include "../sw/npu_tensor_v2.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
_Alignas(32) static unsigned char mem[2*1024*1024];
static void put(unsigned off,int32_t v) { uint32_t x=(uint32_t)v; unsigned i; for(i=0;i<4;i++)mem[off+i]=(unsigned char)(x>>(8*i)); }
static int32_t get(unsigned off) { uint32_t x=0; unsigned i;for(i=0;i<4;i++)x|=(uint32_t)mem[off+i]<<(8*i);return x<=INT32_MAX?(int32_t)x:-1-(int32_t)(UINT32_MAX-x); }
static npu_tensor_job job(unsigned op,unsigned n) { npu_tensor_job j={0};j.opcode=op;j.n=n;j.a=0;j.b=4096;j.c=8192;j.out=12288;j.state=14336;return j; }
static void run(npu_tensor_job *j) { npu_tensor_mapping m={mem,sizeof(mem),0};npu_tensor_stats s;assert(npu_tensor_reference(j,&m,&s)==0);assert(s.error==0); }
static void put16(unsigned off,uint16_t v){mem[off]=(uint8_t)v;mem[off+1]=(uint8_t)(v>>8);}
static int raw_checks(void){
 npu_tensor_job j=job(NPU_TENSOR_REDUCE_RAW_W4,2);npu_tensor_mapping m={mem,sizeof(mem),0};npu_tensor_stats s;npu_tensor_layout l;unsigned r,g;
 j.m=3;j.stride_a=32;j.param0=65536;j.c=0;
 for(r=0;r<2;r++)for(g=0;g<3;g++){put(r*32+g*8,(int32_t)(r|(g<<16)));put(r*32+g*8+4,g?0:r?-127:127);put16(4096+(r*3+g)*2,0x3c00);}
 assert(npu_tensor_reference(&j,&m,&s)==0);assert(get(12288)==65536&&get(12292)==-65536);
 assert(npu_tensor_query(&j,&l)==0&&l.spans[0].bytes==56&&l.spans[1].bytes==12);
 put(12288,12345);put(48,1);assert(npu_tensor_reference(&j,&m,&s)==12&&get(12288)==12345);put(48,1|(2<<16));
 put16(4106,0x7c00);assert(npu_tensor_reference(&j,&m,&s)==4&&s.numeric.status==8&&s.numeric.invalid_inputs==1&&get(12288)==12345);
 j.n=j.m=1;j.param0=1;put(0,0);put(4,1);put16(4096,1);assert(npu_tensor_reference(&j,&m,&s)==0&&get(12288)==0&&s.numeric.underflows==1);
 j.param0=0x80000000;put(4,INT32_MAX);put16(4096,0x7bff);assert(npu_tensor_reference(&j,&m,&s)==0&&get(12288)==INT32_MAX&&s.numeric.saturations==2);
 j.n=4096;j.m=32;j.stride_a=0xffffffe0;assert(npu_tensor_query(&j,&l)==7);
 puts("TENSOR_RAW_W4_CONTRACT_PASS fused_decode tagged_stride invalid_prewrite numeric");return 0;
}
int main(int argc,char **argv) {
 if(argc>1&&strcmp(argv[1],"--raw-only")==0)return raw_checks();
 npu_tensor_job j; npu_tensor_layout l; npu_tensor_mapping m={mem,sizeof(mem),0};npu_tensor_stats s; unsigned i;
 assert(sizeof(npu_tensor_job)==64);
 /* One concentrated contract: all operations, ties, alias/range rejection,
  * state updates and maximum integer values. */
 for(i=1;i<=18;i++){j=job(i,2);j.m=2;j.k=2;j.param0=(i==13||i==14)?1073741824:0;j.param1=i==13?1073741824:0;memset(mem,0,sizeof(mem));put(0,65536);put(4,-65536);put(4096,65536);put(4100,65536);put(8192,65536);put(8196,65536);run(&j);}
 j=job(NPU_TENSOR_MUL,4);put(0,-1);put(4,-3);put(8,1);put(12,3);for(i=0;i<4;i++)put(4096+4*i,32768);run(&j);assert(get(12288)==0&&get(12292)==-2&&get(12296)==0&&get(12300)==2);
 j=job(NPU_TENSOR_ADD,1);put(0,INT32_MAX);put(4096,1);assert(npu_tensor_reference(&j,&m,&s)==0);assert(get(12288)==INT32_MAX&&s.numeric.saturations==1);
 j=job(NPU_TENSOR_SOFTMAX,2);put(0,0);put(4,0);run(&j);assert(get(12288)==32768&&get(12292)==32768);
 j=job(NPU_TENSOR_ROPE,2);put(0,65536);put(4,131072);put(4096,0);put(8192,1073741824);run(&j);assert(get(12288)==-131072&&get(12292)==65536);
 j=job(NPU_TENSOR_RMSNORM,2);j.param0=1;put(0,65536);put(4,-65536);put(4096,65536);put(4100,65536);run(&j);assert(get(12288)==65536&&get(12292)==-65536);
 j=job(NPU_TENSOR_DWCONV,1);j.k=2;put(0,196608);put(4096,65536);put(4100,131072);put(14336,65536);run(&j);assert(get(12288)==458752&&get(14336)==196608);
 j=job(NPU_TENSOR_DELTA,1);j.param0=j.param1=1073741824;put(0,65536);put(4096,65536);put(8192,131072);put(14336,0);run(&j);assert(get(12288)==131072&&get(14336)==33554432);
 j=job(NPU_TENSOR_ATTENTION,2);j.k=1;j.param0=1073741824;put(0,65536);put(4096,0);put(4100,0);put(8192,65536);put(8196,196608);run(&j);assert(get(12288)==131072);
 j=job(NPU_TENSOR_QUANT_A8,3);put(0,INT32_MIN);put(4,0);put(8,INT32_MAX);assert(npu_tensor_reference(&j,&m,&s)==0);assert(mem[12288]==129&&mem[12289]==0&&mem[12290]==127&&s.result==2147483648u);
 j=job(NPU_TENSOR_ARGMAX,3);put(0,7);put(4,8);put(8,8);run(&j);assert(get(12288)==1);
 j=job(NPU_TENSOR_MATVEC_Q16,2);j.k=2;put(0,65536);put(4,131072);put(8,-65536);put(12,0);put(4096,65536);put(4100,196608);run(&j);assert(get(12288)==458752&&get(12292)==-65536);
 j=job(NPU_TENSOR_VECMAT_Q16,2);j.k=2;put(0,65536);put(4,131072);put(4096,65536);put(4100,0);put(4104,196608);put(4108,-65536);run(&j);assert(get(12288)==458752&&get(12292)==-131072);
 j=job(NPU_TENSOR_COPY,2);assert(npu_tensor_query(&j,&l)==0&&l.span_count==2);j.out=0;mem[12288]=99;assert(npu_tensor_reference(&j,&m,&s)==NPU_TENSOR_ERR_OVERLAP);assert(mem[12288]==99);j.out=0x3fffffe0;j.n=9;assert(npu_tensor_reference(&j,&m,&s)==NPU_TENSOR_ERR_RANGE);j=job(1,2);j.stride_a=1;assert(npu_tensor_query(&j,&l)==NPU_TENSOR_ERR_STRIDE);
 j=job(1,2);assert(npu_tensor_reference(&j,&m,(npu_tensor_stats *)(void *)mem)==NPU_TENSOR_ERR_OVERLAP);assert(npu_tensor_query(&j,(npu_tensor_layout *)(void *)&j)==NPU_TENSOR_ERR_OVERLAP);
 /* Maximum dimensions and full-context primitive: do not silently use only
  * the first32 or256 elements. Each final element materially changes output. */
 j=job(NPU_TENSOR_MATVEC_Q16,32);j.k=4096;j.a=65536;j.b=589824;j.out=655360;
 for(i=0;i<32*4096;i++)put(j.a+4*i,0);
 for(i=0;i<4096;i++)put(j.b+4*i,65536);
 for(i=0;i<32;i++)put(j.a+4*(i*4096+4095),(int32_t)(i+1)*65536);
 run(&j);for(i=0;i<32;i++)assert(get(j.out+4*i)==(int32_t)(i+1)*65536);
 j=job(NPU_TENSOR_VECMAT_Q16,32);j.k=256;j.a=65536;j.b=589824;j.out=655360;
 for(i=0;i<32;i++)put(j.a+4*i,i==31?65536:0);
 for(i=0;i<32*256;i++)put(j.b+4*i,0);
 for(i=0;i<256;i++)put(j.b+4*(31*256+i),(int32_t)i*65536);
 run(&j);for(i=0;i<256;i++)assert(get(j.out+4*i)==(int32_t)i*65536);
 j=job(NPU_TENSOR_RMSNORM,4096);j.a=65536;j.b=131072;j.out=196608;j.param0=1;
 for(i=0;i<4096;i++){put(j.a+4*i,INT32_MIN);put(j.b+4*i,65536);}
 run(&j);for(i=0;i<4096;i++)assert(get(j.out+4*i)==-65536);
 j=job(NPU_TENSOR_DELTA,128);j.state=65536;j.param0=j.param1=1073741824;
 for(i=0;i<128;i++){put(j.a+4*i,i==127?65536:0);put(j.b+4*i,i==127?65536:0);put(j.c+4*i,(int32_t)i*256);}
 for(i=0;i<128*128;i++)put(j.state+4*i,0);
 run(&j);for(i=0;i<128;i++)assert(get(j.out+4*i)==(int32_t)i*256);
 /* Successive causal updates change state once per call and preserve values
  * for beta=1 when the same key/value is presented again. */
 run(&j);for(i=0;i<128;i++)assert(get(j.out+4*i)==(int32_t)i*256);
 j=job(NPU_TENSOR_DWCONV,1);j.k=3;put(4096,65536);put(4100,131072);put(4104,196608);put(14336,0);put(14340,0);
 for(i=1;i<=4;i++){put(0,(int32_t)i*65536);run(&j);assert(get(14340)==(int32_t)i*65536);assert(get(14336)==(int32_t)(i-1)*65536);assert(get(12288)==(i==1?3:i==2?8:(int32_t)(6*i-4))*65536);}
 /* All validation failures preserve the entire DDR, including state. */
 j=job(1,1);j.n=0;j.flags=1;j.out=1;assert(npu_tensor_reference(&j,&m,&s)==3&&s.error==3);j.n=1;assert(npu_tensor_reference(&j,&m,&s)==5);j.flags=0;assert(npu_tensor_reference(&j,&m,&s)==6);j.out=0x40000000;assert(npu_tensor_reference(&j,&m,&s)==7);
 j=job(1,1);{npu_tensor_mapping shortmap={mem,4,0};assert(npu_tensor_reference(&j,&shortmap,&s)==9);}
 j=job(1,1);{npu_tensor_mapping shifted={mem+32,1024,32};j.a=32;j.out=64;put(32,12345);assert(npu_tensor_reference(&j,&shifted,&s)==0&&get(64)==12345);}
 j=job(1,1);{npu_tensor_job *in_ddr=(npu_tensor_job *)(void *)(mem+16384);*in_ddr=j;in_ddr->out=16384;assert(npu_tensor_reference(in_ddr,&m,&s)==8);assert(in_ddr->opcode==1);}
 j=job(1,1);{npu_tensor_mapping *in_ddr=(npu_tensor_mapping *)(void *)(mem+16384);*in_ddr=m;j.out=16384;assert(npu_tensor_reference(&j,in_ddr,&s)==8);assert(in_ddr->data==m.data);}
 puts("TENSOR_REFERENCE_CONTRACT_PASS all18 ties state spans alias");return 0;
}
