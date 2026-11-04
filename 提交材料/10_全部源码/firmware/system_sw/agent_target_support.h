#ifndef AGENT_TARGET_SUPPORT_H
#define AGENT_TARGET_SUPPORT_H
#ifndef __ASSEMBLER__
#include <stddef.h>
/* Additional freestanding libc primitives used by the Agent protocol code. */
char *strstr(const char *haystack,const char *needle);
char *strcpy(char *destination,const char *source);
int strncmp(const char *left,const char *right,size_t bytes);
void *memchr(const void *data,int byte,size_t bytes);
#endif
#endif
