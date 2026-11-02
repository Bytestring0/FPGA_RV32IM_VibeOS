#include "qw_json.h"
#include <string.h>
static void ws(qj_doc d,size_t*p){while(*p<d.n&&(d.s[*p]==' '||d.s[*p]=='\r'||d.s[*p]=='\n'||d.s[*p]=='\t'))++*p;}
static int hex(char c){return(c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f');}
static int skip(qj_doc d,size_t*p,unsigned depth){
 if(depth>32)return -1;ws(d,p);if(*p>=d.n)return -1;
 char c=d.s[(*p)++];
 if(c=='"'){
  while(*p<d.n){c=d.s[(*p)++];if(c=='"')return 0;if((unsigned char)c<32)return -1;
   if(c=='\\'){if(*p>=d.n)return -1;c=d.s[(*p)++];
    if(c=='u'){for(unsigned i=0;i<4;i++)if(*p>=d.n||!hex(d.s[(*p)++]))return -1;}
    else if(!strchr("\"\\/bfnrt",c))return -1;
   }
  }return -1;
 }
 if(c=='{'||c=='['){
  char end=c=='{'?'}':']';ws(d,p);if(*p<d.n&&d.s[*p]==end){++*p;return 0;}
  for(;;){if(c=='{'){ws(d,p);if(*p>=d.n||d.s[*p]!='"'||skip(d,p,depth+1))return -1;ws(d,p);if(*p>=d.n||d.s[(*p)++]!=':')return -1;}
   if(skip(d,p,depth+1))return -1;ws(d,p);if(*p>=d.n)return -1;
   char sep=d.s[(*p)++];if(sep==end)return 0;if(sep!=',')return -1;
  }
 }
 const char*lit=c=='n'?"ull":c=='t'?"rue":c=='f'?"alse":0;
 if(lit){while(*lit){if(*p>=d.n||d.s[(*p)++]!=*lit++)return -1;}return 0;}
 --*p;if(d.s[*p]=='-')++*p;if(*p>=d.n)return -1;
 if(d.s[*p]=='0')++*p;else{if(d.s[*p]<'1'||d.s[*p]>'9')return -1;while(*p<d.n&&d.s[*p]>='0'&&d.s[*p]<='9')++*p;}
 if(*p<d.n&&d.s[*p]=='.'){++*p;size_t a=*p;while(*p<d.n&&d.s[*p]>='0'&&d.s[*p]<='9')++*p;if(a==*p)return -1;}
 if(*p<d.n&&(d.s[*p]=='e'||d.s[*p]=='E')){++*p;if(*p<d.n&&(d.s[*p]=='+'||d.s[*p]=='-'))++*p;size_t a=*p;while(*p<d.n&&d.s[*p]>='0'&&d.s[*p]<='9')++*p;if(a==*p)return -1;}
 return 0;
}
int qj_read(qj_doc d,size_t*p,qj_value*v){
 if(!p||!v||!d.s)return -1;ws(d,p);if(*p>=d.n)return -1;
 v->begin=*p;v->kind=d.s[*p];if(skip(d,p,0))return -1;v->end=*p;ws(d,p);v->next=*p;return 0;
}
int qj_text(qj_doc d,qj_value v,char*out,size_t cap){
 if(v.kind!='"'||!out||!cap||v.end>d.n)return -1;size_t n=0;
 for(size_t p=v.begin+1;p+1<v.end;p++){
  unsigned char c=(unsigned char)d.s[p];
  if(c=='\\'){c=(unsigned char)d.s[++p];if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='u')return -1;}
  if(!c||n+1>=cap)return -1;out[n++]=(char)c;
 }out[n]=0;return 0;
}
int qj_get(qj_doc d,qj_value object,const char*key,qj_value*out){
 if(object.kind!='{'||object.end>d.n)return -1;size_t p=object.begin+1;int found=0;
 while(p<object.end-1){qj_value k,v;char name[96];ws(d,&p);if(p==object.end-1)break;
  if(qj_read(d,&p,&k)||qj_text(d,k,name,sizeof name)||p>=d.n||d.s[p++]!=':'||qj_read(d,&p,&v))return -1;
  if(!strcmp(name,key)){if(found)return -1;*out=v;found=1;}
  if(p<object.end-1){if(d.s[p++]!=',')return -1;}
 }return found?0:1;
}
int qj_uint(qj_doc d,qj_value v,uint64_t*out){
 if(!out||v.begin>=v.end||v.end>d.n)return -1;uint64_t x=0;
 for(size_t p=v.begin;p<v.end;p++){unsigned c=(unsigned char)d.s[p];if(c<'0'||c>'9'||x>(UINT64_MAX-(c-'0'))/10)return -1;x=x*10+c-'0';}
 *out=x;return 0;
}
