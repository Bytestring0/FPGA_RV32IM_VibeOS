#ifndef AG_SERVICES_H
#define AG_SERVICES_H
#include <stddef.h>
#include <stdint.h>
/* Software contracts, no MMIO/pin addresses and no pretend implementations.
 * All callbacks return 0 on success, nonzero on error, with bounded waits.
 * File implementation owns offset/range checks and durable sync semantics.
 */
typedef struct {
    void *context;
    int (*read)(void *, const char *path, uint64_t offset, void *dst,
                size_t capacity, size_t *actual);
    int (*replace)(void *, const char *path, const void *data, size_t length);
    int (*sync)(void *);
} ag_files;
typedef struct {
    void *context;
    int (*fill_rect)(void *, unsigned x, unsigned y, unsigned w, unsigned h, uint16_t rgb565);
    int (*text)(void *, unsigned x, unsigned y, const char *utf8, size_t bytes);
    /* present only after CPU writes are visible to scanout (Cache maintenance).
     * Return only once buffer ownership transfer is known, or report pending
     * through a concrete adapter outside this synchronous v0 contract.
     */
    int (*present)(void *);
} ag_display;
typedef struct {
    void *context;
    int (*load)(void *, const char *manifest_path);
    int (*prefill)(void *, const uint32_t *tokens, size_t count);
    int (*decode)(void *, uint32_t *token, int *finished);
    int (*reset)(void *);
} ag_inference;
#endif
