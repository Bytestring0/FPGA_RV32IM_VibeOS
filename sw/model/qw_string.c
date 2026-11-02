#if defined(QW_FREESTANDING)
#include <stddef.h>
/* memcpy/memset/memcmp are supplied by the unchanged NPU freestanding.c. */
size_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return(unsigned char)*a-(unsigned char)*b;}
int strncmp(const char*a,const char*b,size_t n){for(size_t i=0;i<n;i++){unsigned char x=(unsigned char)a[i],y=(unsigned char)b[i];if(x!=y||!x)return(int)x-(int)y;}return 0;}
char *strstr(const char*s,const char*needle){if(!*needle)return(char*)s;for(;*s;s++){size_t i=0;while(needle[i]&&s[i]&&needle[i]==s[i])i++;if(!needle[i])return(char*)s;}return 0;}
char *strchr(const char*s,int c){for(;;s++){if((unsigned char)*s==(unsigned char)c)return(char*)s;if(!*s)return 0;}}
void *memmove(void*d,const void*s,size_t n){unsigned char*to=d;const unsigned char*from=s;if((size_t)to<=(size_t)from){for(size_t i=0;i<n;i++)to[i]=from[i];}else while(n){n--;to[n]=from[n];}return d;}
#endif
