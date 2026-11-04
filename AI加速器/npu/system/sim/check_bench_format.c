#include <stdio.h>
#include <stdint.h>
#include <string.h>
static char *put_num(char *d,uint64_t v){char t[24];unsigned n=0;if(!v){*d++='0';return d;}while(v){t[n++]=(char)('0'+v%10);v/=10;}while(n)*d++=t[--n];return d;}
static char *put_milli(char *d,uint64_t v){d=put_num(d,v/1000);*d++='.';d[0]=(char)('0'+v/100%10);d[1]=(char)('0'+v/10%10);d[2]=(char)('0'+v%10);return d+3;}
int main(void){uint64_t cycles=0x764308F3ull;unsigned iters=10000;uint64_t HZ=102000000ull;
 char buf[256];char*d=buf;
 d=put_num(d,cycles*1000u/HZ); // seconds milli
 printf("Seconds: %.*s\n",(int)(d-buf),buf);
 uint64_t milli=(uint64_t)iters*HZ*1000u/cycles;
 d=buf;d=put_milli(d,milli);printf("CoreMark: %.*s\n",(int)(d-buf),buf);
 uint64_t mhz=HZ/1000000u;d=buf;d=put_milli(d,milli/mhz);printf("CoreMark/MHz: %.*s\n",(int)(d-buf),buf);
 return 0;}