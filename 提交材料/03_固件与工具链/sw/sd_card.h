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

/* No allocation and no DMA: destination is written by 32-bit PIO reads.
 * SD_SPI_HW_CRC=1 (default) uses CAP bit2 RXBURST CRC16 when available;
 * CAP=3 controllers retain software CRC16, or force it with SD_SPI_HW_CRC=0.
 * SD_SPI_MULTIBLOCK=1 opts into CMD18; explicit illegal-command response
 * falls back to CMD17 until reinit. All other errors invalidate this read.
 * On any read error, destination may contain partial, untrusted data. */
typedef struct {
    uint64_t read_calls;
    uint64_t blocks_requested;
    uint64_t blocks_completed;
    uint64_t crc_verified;
    uint64_t crc_hardware;
    uint64_t crc_software;
    uint64_t cmd17;
    uint64_t cmd18;
    uint64_t cmd12;
    uint64_t cmd18_fallback;
    uint64_t errors;
    uint32_t crc_mode; /* 0=uninitialized, 1=software, 2=hardware */
} sd_card_stats;

/* Single-card, non-reentrant snapshots; NULL is ignored. Counters wrap modulo
 * 2^64, persist across init, and reset never touches card/MMIO state.
 * read_calls includes rejected/zero-length calls; requested counts validated
 * nonzero reads. completed requires full call success; verified includes any
 * sectors checked before a later failure. crc_hardware/software count attempts. */
void sd_card_get_stats(sd_card_stats *out);
void sd_card_reset_stats(void);
int sd_card_init(uintptr_t base, uint32_t timeout);
int sd_card_read_blocks(uintptr_t base, uint32_t lba, uint32_t count,
                        void *destination, uint32_t timeout);
uint8_t sd_crc7(const uint8_t *data, uint32_t length);

uint16_t sd_crc16(const uint8_t *data, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
