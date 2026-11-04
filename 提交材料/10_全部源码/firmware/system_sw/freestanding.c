#include <stddef.h>
/* Freestanding compiler support for struct copies/zero initializers.
 * Volatile byte access prevents optimization back into an unresolved builtin. */
void *memcpy(void *destination,const void *source,size_t count) {
    volatile unsigned char *d=(volatile unsigned char *)destination;
    const volatile unsigned char *s=(const volatile unsigned char *)source;
    for(size_t i=0;i<count;i++)d[i]=s[i];
    return destination;
}
void *memset(void *destination,int value,size_t count) {
    volatile unsigned char *d=(volatile unsigned char *)destination;
    for(size_t i=0;i<count;i++)d[i]=(unsigned char)value;
    return destination;
}
int memcmp(const void *left,const void *right,size_t count) {
    const volatile unsigned char *a=(const volatile unsigned char *)left;
    const volatile unsigned char *b=(const volatile unsigned char *)right;
    for(size_t i=0;i<count;i++)if(a[i]!=b[i])return a[i]<b[i]?-1:1;
    return 0;
}
