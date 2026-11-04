#ifndef QW_SHA256_H
#define QW_SHA256_H
#include <stdint.h>
#include <stddef.h>
typedef struct {uint32_t h[8];uint64_t bytes;unsigned used;unsigned char block[64];} qw_sha256;
void qw_sha_init(qw_sha256*);
void qw_sha_update(qw_sha256*,const void*,size_t);
void qw_sha_final(qw_sha256*,unsigned char[32]);
#endif
