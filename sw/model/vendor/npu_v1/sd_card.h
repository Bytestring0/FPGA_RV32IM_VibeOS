#ifndef SD_CARD_H
#define SD_CARD_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_SPI_CSR_BASE 0x10005000u
#define SD_OK             0
#define SD_E_ARG         -1
#define SD_E_TIMEOUT     -2
#define SD_E_NO_CARD     -3
#define SD_E_R1          -4
#define SD_E_CRC         -5
#define SD_E_TOKEN       -6
#define SD_E_BUS         -7
#define SD_E_CAPACITY    -8

/* No allocation and no DMA: destination is written by 32-bit PIO reads. */
int sd_card_init(uintptr_t base, uint32_t timeout);
int sd_card_read_blocks(uintptr_t base, uint32_t lba, uint32_t count,
                        void *destination, uint32_t timeout);
uint8_t sd_crc7(const uint8_t *data, uint32_t length);
uint16_t sd_crc16(const uint8_t *data, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
