#ifndef QW_JSON_H
#define QW_JSON_H
#include <stddef.h>
#include <stdint.h>
typedef struct {const char *s;size_t n;} qj_doc;
typedef struct {size_t begin,end,next;char kind;} qj_value;
int qj_read(qj_doc,size_t *cursor,qj_value *);
int qj_get(qj_doc,qj_value object,const char *key,qj_value *);
int qj_text(qj_doc,qj_value,char *out,size_t capacity);
int qj_uint(qj_doc,qj_value,uint64_t *);
#endif
