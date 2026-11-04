#include <stddef.h>
#include <stdint.h>
typedef uint32_t copy_word __attribute__((__may_alias__));
void *memcpy(void *destination,const void *source,size_t count) {
    volatile unsigned char *d=destination;
    const volatile unsigned char *s=source;
    if ((((uintptr_t)d^(uintptr_t)s)&3u)==0u) {
        while(count&&((uintptr_t)d&3u)){*d++=*s++;--count;}
        volatile copy_word *dw=(volatile copy_word *)d;
        const volatile copy_word *sw=(const volatile copy_word *)s;
        while(count>=16u){uint32_t a=sw[0],b=sw[1],c=sw[2],e=sw[3];dw[0]=a;dw[1]=b;dw[2]=c;dw[3]=e;dw+=4;sw+=4;count-=16u;}
        while(count>=4u){*dw++=*sw++;count-=4u;}
        d=(volatile unsigned char *)dw;s=(const volatile unsigned char *)sw;
    }
    while(count--)*d++=*s++;
    return destination;
}
void *memset(void *destination,int value,size_t count) {
    volatile unsigned char *d=destination;uint32_t byte=(unsigned char)value;
    while(count&&((uintptr_t)d&3u)){*d++=(unsigned char)byte;--count;}
    uint32_t word=byte*UINT32_C(0x01010101);
    volatile copy_word *dw=(volatile copy_word *)d;
    while(count>=16u){dw[0]=word;dw[1]=word;dw[2]=word;dw[3]=word;dw+=4;count-=16u;}
    while(count>=4u){*dw++=word;count-=4u;}
    d=(volatile unsigned char *)dw;
    while(count--)*d++=(unsigned char)byte;
    return destination;
}
int memcmp(const void *left,const void *right,size_t count) {
    const volatile unsigned char *a=left,*b=right;
    while(count--){unsigned char x=*a++,y=*b++;if(x!=y)return x<y?-1:1;}
    return 0;
}
