#include "sd_spi.h"
#include "../platform.h"

#define SD_SPI_DATA     0x10001000u
#define SD_SPI_STATUS   0x10001004u
#define SD_SPI_CONTROL  0x10001008u
#define SD_SPI_DIVIDER  0x1000100cu

#define SD_STATUS_BUSY      (1u << 0)
#define SD_STATUS_RX_VALID  (1u << 1)
#define SD_STATUS_ERROR     (1u << 2)
#define SD_REG_TIMEOUT      1000000u
#define SD_TOKEN_TIMEOUT    65536u

#define SD_CMD0   0u
#define SD_CMD8   8u
#define SD_CMD16  16u
#define SD_CMD17  17u
#define SD_CMD55  55u
#define SD_CMD58  58u
#define SD_ACMD41 41u

static int sd_high_capacity;

static int spi_transfer(uint8_t tx, uint8_t *rx) {
    uint32_t status = MMIO32(SD_SPI_STATUS);
    if ((status & SD_STATUS_BUSY) || (status & SD_STATUS_ERROR)) return -1;
    MMIO32(SD_SPI_DATA) = tx;
    for (uint32_t i = 0; i < SD_REG_TIMEOUT; ++i) {
        status = MMIO32(SD_SPI_STATUS);
        if (!(status & SD_STATUS_BUSY)) {
            if (status & SD_STATUS_ERROR) return -1;
            if (!(status & SD_STATUS_RX_VALID)) return -1;
            *rx = (uint8_t)MMIO32(SD_SPI_DATA);
            return 0;
        }
    }
    return -1;
}

static void sd_select(void) { MMIO32(SD_SPI_CONTROL) = 0; }

static void sd_release(void) {
    uint8_t ignored;
    MMIO32(SD_SPI_CONTROL) = 1;
    (void)spi_transfer(0xff, &ignored);
}

static int sd_crc7(const uint8_t frame[5]) {
    uint8_t crc = 0;
    for (unsigned i = 0; i < 5; ++i) {
        uint8_t data = frame[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if (((data ^ crc) & 0x80u) != 0) crc ^= 0x09u;
            data <<= 1;
        }
    }
    return (int)((crc << 1) | 1u);
}

static int sd_send_command(uint8_t command, uint32_t argument,
                           uint8_t *r1, uint8_t *extra, unsigned extra_count) {
    uint8_t frame[5] = {
        (uint8_t)(0x40u | command),
        (uint8_t)(argument >> 24), (uint8_t)(argument >> 16),
        (uint8_t)(argument >> 8), (uint8_t)argument
    };
    uint8_t byte = 0xff;
    unsigned ready_attempt;
    for (ready_attempt = 0; ready_attempt < 8; ++ready_attempt) {
        if (spi_transfer(0xff, &byte) != 0) return -1;
        if (byte == 0xff) break;
    }
    if (ready_attempt == 8) return -1;
    for (unsigned i = 0; i < sizeof(frame); ++i)
        if (spi_transfer(frame[i], &byte) != 0) return -1;
    if (spi_transfer((uint8_t)sd_crc7(frame), &byte) != 0) return -1;
    for (unsigned i = 0; i < 10; ++i) {
        if (spi_transfer(0xff, &byte) != 0) return -1;
        if (!(byte & 0x80u)) {
            *r1 = byte;
            if (byte != 0x05) {
                for (unsigned j = 0; j < extra_count; ++j)
                    if (spi_transfer(0xff, &extra[j]) != 0) return -1;
            }
            return 0;
        }
    }
    return -1;
}

static sd_spi_result sd_command(uint8_t command, uint32_t argument,
                                uint8_t *r1, uint8_t *extra,
                                unsigned extra_count) {
    sd_select();
    int result = sd_send_command(command, argument, r1, extra, extra_count);
    sd_release();
    return result == 0 ? SD_SPI_OK : SD_SPI_TIMEOUT;
}

static int sd_app_command(uint32_t argument, uint8_t *r1) {
    uint8_t prefix;
    sd_select();
    int result = sd_send_command(SD_CMD55, 0, &prefix, 0, 0);
    if (result == 0 && prefix <= 1)
        result = sd_send_command(SD_ACMD41, argument, r1, 0, 0);
    else if (result == 0) result = -2;
    sd_release();
    return result;
}

static uint16_t sd_crc16(const uint8_t *data, unsigned length) {
    uint16_t crc = 0;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

sd_spi_result sd_spi_card_init(void) {
    uint8_t response, r7[4], ocr[4];
    sd_high_capacity = 0;
    MMIO32(SD_SPI_CONTROL) = 1;
    MMIO32(SD_SPI_DIVIDER) = 127;
    MMIO32(SD_SPI_STATUS) = SD_STATUS_ERROR;
    for (unsigned i = 0; i < 10; ++i)
        if (spi_transfer(0xff, &response) != 0) return SD_SPI_IO;

    unsigned attempts;
    for (attempts = 0; attempts < 10; ++attempts) {
        if (sd_command(SD_CMD0, 0, &response, 0, 0) != SD_SPI_OK) return SD_SPI_TIMEOUT;
        if (response == 0x01) break;
    }
    if (attempts == 10) return SD_SPI_PROTOCOL;

    int version2 = 0;
    if (sd_command(SD_CMD8, 0x000001aau, &response, r7, sizeof(r7)) != SD_SPI_OK)
        return SD_SPI_TIMEOUT;
    if (response == 0x01) {
        if (r7[2] != 0x01 || r7[3] != 0xaa) return SD_SPI_UNSUPPORTED;
        version2 = 1;
    } else if (response != 0x05) return SD_SPI_PROTOCOL;

    for (attempts = 0; attempts < 1000; ++attempts) {
        int command_result = sd_app_command(version2 ? 0x40000000u : 0, &response);
        if (command_result == -1) return SD_SPI_TIMEOUT;
        if (command_result != 0) return SD_SPI_PROTOCOL;
        if (response == 0) break;
        if (response != 1) return SD_SPI_PROTOCOL;
    }
    if (attempts == 1000) return SD_SPI_TIMEOUT;

    if (sd_command(SD_CMD58, 0, &response, ocr, sizeof(ocr)) != SD_SPI_OK)
        return SD_SPI_TIMEOUT;
    if (response != 0) return SD_SPI_PROTOCOL;
    if (!(ocr[0] & 0x80u)) return SD_SPI_PROTOCOL;
    sd_high_capacity = (ocr[0] & 0x40u) != 0;
    if (!sd_high_capacity) {
        if (sd_command(SD_CMD16, SD_SPI_SECTOR_SIZE, &response, 0, 0) != SD_SPI_OK)
            return SD_SPI_TIMEOUT;
        if (response != 0) return SD_SPI_PROTOCOL;
    }
    MMIO32(SD_SPI_DIVIDER) = 7;
    return SD_SPI_OK;
}

sd_spi_result sd_spi_read_sector(uint32_t lba, uint8_t sector[SD_SPI_SECTOR_SIZE]) {
    if (!sector) return SD_SPI_RANGE;
    if (!sd_high_capacity && lba > 0x007fffffu) return SD_SPI_RANGE;
    uint32_t argument = sd_high_capacity ? lba : (lba << 9);
    uint8_t response, token = 0xff, crc_hi, crc_lo;
    sd_select();
    if (sd_send_command(SD_CMD17, argument, &response, 0, 0) != 0) {
        sd_release();
        return SD_SPI_TIMEOUT;
    }
    if (response != 0) {
        sd_release();
        return SD_SPI_PROTOCOL;
    }
    unsigned wait;
    for (wait = 0; wait < SD_TOKEN_TIMEOUT; ++wait) {
        if (spi_transfer(0xff, &token) != 0) {
            sd_release();
            return SD_SPI_IO;
        }
        if (token != 0xff) break;
    }
    if (token != 0xfe) {
        sd_release();
        return token == 0xff ? SD_SPI_TIMEOUT : SD_SPI_PROTOCOL;
    }
    for (unsigned i = 0; i < SD_SPI_SECTOR_SIZE; ++i) {
        if (spi_transfer(0xff, &sector[i]) != 0) {
            sd_release();
            return SD_SPI_IO;
        }
    }
    if (spi_transfer(0xff, &crc_hi) != 0 || spi_transfer(0xff, &crc_lo) != 0) {
        sd_release();
        return SD_SPI_IO;
    }
    sd_release();
    uint16_t received_crc = (uint16_t)(((uint16_t)crc_hi << 8) | crc_lo);
    if (received_crc != sd_crc16(sector, SD_SPI_SECTOR_SIZE)) return SD_SPI_CRC;
    return SD_SPI_OK;
}
