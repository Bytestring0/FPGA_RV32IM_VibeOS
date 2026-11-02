#ifndef QW_TOKENIZER_H
#define QW_TOKENIZER_H
#include <stddef.h>
#include <stdint.h>
typedef struct {const unsigned char *data;size_t bytes;uint32_t h[24];} qw_tokenizer;
/* TOKEN.BIN is immutable caller-owned memory. Verification includes its exact
 * SHA256 and bounds. No target Python/regex/heap dependency. */
int qt_open(qw_tokenizer *,const void *,size_t,const unsigned char expected_sha256[32]);
size_t qt_workspace_bytes(size_t maximum_input_bytes);
/* NFC + frozen Qwen regex + byte-level BPE. parse_special=0 still recognizes
 * tokenizer added tokens whose special flag is false, matching the capture
 * convention. Framing inserts canonical special IDs separately. */
int qt_encode(const qw_tokenizer *,const char *,size_t,int parse_special,
 uint32_t *tokens,size_t capacity,size_t *count,void *workspace,size_t bytes);
int qt_piece(const qw_tokenizer *,uint32_t token,const char **bytes,size_t *count,
 unsigned *flags);
#endif
