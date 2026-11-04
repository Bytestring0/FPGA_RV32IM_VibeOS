#ifndef AG_ASSET_LOADER_H
#define AG_ASSET_LOADER_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
 void *user;
 int (*receive)(void *,unsigned char *,uint32_t timeout_ms);
 int (*send)(void *,const unsigned char *,size_t);
} ag_asset_transport;
/* Receiver accepts only the provided destination and exact size. Stop-and-wait
 * 256B packets carry offsets and CRC32; an asset-wide CRC gates completion. */
int ag_asset_receive(const ag_asset_transport *,void *destination,uint32_t bytes);
#endif
