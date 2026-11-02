#include "agent_target_support.h"
char *strstr(const char *haystack,const char *needle){
    if(!*needle)return(char*)haystack;
    for(;*haystack;haystack++){
        const char *a=haystack,*b=needle;
        while(*b&&*a==*b){a++;b++;}
        if(!*b)return(char*)haystack;
    }
    return 0;
}
char *strcpy(char *destination,const char *source){
    char *begin=destination;
    do{*destination++=*source;}while(*source++);
    return begin;
}
int strncmp(const char *left,const char *right,size_t bytes){
    while(bytes--){unsigned char a=(unsigned char)*left++,b=(unsigned char)*right++;if(a!=b)return(int)a-(int)b;if(!a)return 0;}
    return 0;
}
void *memchr(const void *data,int byte,size_t bytes){
    const unsigned char*p=data;unsigned char wanted=(unsigned char)byte;
    while(bytes--){if(*p==wanted)return(void*)p;p++;}
    return 0;
}
