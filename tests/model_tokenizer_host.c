#include "qw_tokenizer.h"
#include "qw_runtime.h"
#include "qw_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static uint32_t read32(FILE*f){unsigned char p[4];assert(fread(p,1,4,f)==4);return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int main(int argc,char**argv){
 if(argc!=3)return 2;FILE*f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0);rewind(f);unsigned char*blob=malloc((size_t)size);assert(blob&&fread(blob,1,(size_t)size,f)==(size_t)size);fclose(f);
 unsigned char sha[32];qw_sha256 h;qw_sha_init(&h);qw_sha_update(&h,blob,(size_t)size);qw_sha_final(&h,sha);qw_tokenizer t;assert(!qt_open(&t,blob,(size_t)size,sha));
 size_t wbytes=qt_workspace_bytes(32768);void*work=malloc(wbytes);uint32_t*out=malloc(32768*sizeof(uint32_t)),*expected=malloc(32768*sizeof(uint32_t));unsigned char*input=malloc(32769);assert(work&&out&&expected&&input);
 f=fopen(argv[2],"rb");assert(f);uint32_t cases=read32(f);unsigned tested=0;
 for(uint32_t i=0;i<cases;i++){
  uint32_t bytes=read32(f),special=read32(f),wanted=read32(f);assert(bytes<=32768&&wanted<=32768&&fread(input,1,bytes,f)==bytes);for(unsigned j=0;j<wanted;j++)expected[j]=read32(f);
  size_t count=0;int rc=qt_encode(&t,(const char*)input,bytes,(int)special,out,32768,&count,work,wbytes);
  if(rc||count!=wanted||memcmp(out,expected,wanted*sizeof(uint32_t))){fprintf(stderr,"TOKENIZER FAIL case=%u bytes=%u mode=%u rc=%d count=%zu wanted=%u\n",i,bytes,special,rc,count,wanted);for(unsigned j=0;j<count&&j<32;j++)fprintf(stderr,"%u,",out[j]);fputs("\nexpected:",stderr);for(unsigned j=0;j<wanted&&j<32;j++)fprintf(stderr,"%u,",expected[j]);fputc('\n',stderr);return 1;}
  if(bytes&&wanted){size_t n=0;assert(qt_encode(&t,(const char*)input,bytes,(int)special,out,wanted-1,&n,work,wbytes)==QW_CAPACITY);}
  tested++;
 }
 fclose(f);const char bad[][5]={{(char)0xc0,(char)0xaf},{(char)0xed,(char)0xa0,(char)0x80},{(char)0xf4,(char)0x90,(char)0x80,(char)0x80}};const unsigned lengths[]={2,3,4};
 for(unsigned i=0;i<3;i++){size_t n;assert(qt_encode(&t,bad[i],lengths[i],0,out,32,&n,work,wbytes)==QW_TOKEN);}
 sha[0]^=1;assert(qt_open(&t,blob,(size_t)size,sha)==QW_HASH);
 printf("C TOKENIZER PASS %u HF-reference cases + capacity/invalid UTF8/hash guards; workspace=%zu\n",tested,wbytes);
 free(input);free(expected);free(out);free(work);free(blob);return 0;
}
