#ifndef AG_SD_SPI_H
#define AG_SD_SPI_H
#include "../platform.h"
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;

#define SD_SPI_SECTOR_SIZE 512u

typedef enum {
    SD_SPI_OK = 0,
    SD_SPI_TIMEOUT = -1,
    SD_SPI_PROTOCOL = -2,
    SD_SPI_UNSUPPORTED = -3,
    SD_SPI_CRC = -4,
    SD_SPI_RANGE = -5,
    SD_SPI_IO = -6
} sd_spi_result;

/* Read-only SPI-mode SD card access through ag_soc MMIO. */
sd_spi_result sd_spi_card_init(void);
sd_spi_result sd_spi_read_sector(uint32_t lba, uint8_t sector[SD_SPI_SECTOR_SIZE]);

#endif
