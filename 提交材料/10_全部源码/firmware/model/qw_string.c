#if defined(QW_FREESTANDING)
#include <stddef.h>
/* memcpy/memset/memcmp are supplied by the unchanged NPU freestanding.c. */
size_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return(unsigned char)*a-(unsigned char)*b;}
char *strchr(const char*s,int c){for(;;s++){if((unsigned char)*s==(unsigned char)c)return(char*)s;if(!*s)return 0;}}
void *memmove(void*d,const void*s,size_t n){unsigned char*to=d;const unsigned char*from=s;if((size_t)to<=(size_t)from){for(size_t i=0;i<n;i++)to[i]=from[i];}else while(n){n--;to[n]=from[n];}return d;}
#endif
