/* Export a versioned complete prefix using the existing real C tokenizer. */
#include "qw_tokenizer.h"
#include "qw_sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *load(const char*path,size_t*n){FILE*f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0);rewind(f);void*p=malloc((size_t)size);assert(p&&fread(p,1,(size_t)size,f)==(size_t)size);assert(!fclose(f));*n=(size_t)size;return p;}
static unsigned hex(char c){return c>='0'&&c<='9'?(unsigned)(c-'0'):(unsigned)(c-'a'+10);}
static void hash(const void*p,size_t n,unsigned char out[32]){qw_sha256 h;qw_sha_init(&h);qw_sha_update(&h,p,n);qw_sha_final(&h,out);}
static void show(const unsigned char h[32]){for(unsigned i=0;i<32;i++)printf("%02x",h[i]);}
static void put32(FILE*f,uint32_t x){unsigned char bytes[4]={(unsigned char)x,(unsigned char)(x>>8),(unsigned char)(x>>16),(unsigned char)(x>>24)};assert(fwrite(bytes,1,4,f)==4);}
int main(int argc,char**argv){
 if(argc!=4){fprintf(stderr,"usage: model_prefix_export_host TOKEN.BIN prefix.txt out_ids.u32le.bin\n");return 2;}
 size_t bytes,n;void*blob=load(argv[1],&bytes);char*text=load(argv[2],&n);assert(n<=16384);
 const char*expected="66df043b96ef2ccaa8d6e24148b0dc03eb022e8f461a88fdc527c178cbe4aa45";
 unsigned char digest[32];for(unsigned i=0;i<32;i++)digest[i]=(unsigned char)(hex(expected[i*2])*16+hex(expected[i*2+1]));
 qw_tokenizer tok;assert(!qt_open(&tok,blob,bytes,digest));
 size_t workspace_bytes=qt_workspace_bytes(n);void*workspace=malloc(workspace_bytes);assert(workspace);
 uint32_t ids[2048];size_t count=0;int rc=qt_encode(&tok,text,n,1,ids,2048,&count,workspace,workspace_bytes);
 if(rc||!count||count>=2048){fprintf(stderr,"PREFIX_CAPACITY_OR_TOKENIZER_ERROR rc=%d tokens=%zu\n",rc,count);return 1;}
 size_t decoded=0;for(size_t i=0;i<count;i++){const char*piece;size_t len;unsigned flags;assert(!qt_piece(&tok,ids[i],&piece,&len,&flags));
  if(len>n-decoded||memcmp(text+decoded,piece,len)){fprintf(stderr,"PREFIX_UTF8_ROUNDTRIP_MISMATCH\n");return 1;}decoded+=len;}
 assert(decoded==n);FILE*out=fopen(argv[3],"wb");assert(out);put32(out,(uint32_t)count);for(size_t i=0;i<count;i++)put32(out,ids[i]);assert(!fclose(out));
 hash(text,n,digest);printf("PREFIX_BYTES=%zu PREFIX_TOKENS=%zu UTF8_SHA256=",n,count);show(digest);puts("");
 hash(ids,count*sizeof(uint32_t),digest);printf("TOKEN_IDS_SHA256=");show(digest);puts(" C_TOKENIZER_ENCODE_DECODE_EXACT");
 free(workspace);free(text);free(blob);return 0;
}
