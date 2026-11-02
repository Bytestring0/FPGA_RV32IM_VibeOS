#include "sd_card.h"

/* MMIO registers; transactions are full 32-bit words. */
#define CTRL      0x00u
#define STATUS    0x04u
#define DIVIDER   0x08u
#define TIMEOUT   0x0cu
#define TXBYTE    0x10u
#define RXBYTE    0x14u
#define RXCOUNT   0x18u
#define ID        0x1cu
#define CAP       0x20u
#define RXWORD    0x24u
#define RXBURST   0x28u
#define BUSY      0x01u
#define PRESENT   0x04u
#define FAULTS    0xa8u /* timeout, card removed, rejected transfer */

#ifndef SD_SPI_CLOCK_HZ
#define SD_SPI_CLOCK_HZ 102000000u
#endif
#ifndef SD_SPI_INIT_DIVIDER
#define SD_SPI_INIT_DIVIDER ((SD_SPI_CLOCK_HZ + 799999u) / 800000u) /* ceil(clock/800 kHz): 128 at 102 MHz */
#endif
#ifndef SD_SPI_DATA_DIVIDER
#define SD_SPI_DATA_DIVIDER 4u /* >=4: 12.75 MHz at 102 MHz, MISO sync margin */
#endif

static uintptr_t initialized_base;
static uint8_t initialized;
static uint8_t high_capacity;

#ifdef SD_SPI_TEST_MMIO
extern uint32_t sd_test_mmio_read(uintptr_t base, uint32_t reg);
extern void sd_test_mmio_write(uintptr_t base, uint32_t reg, uint32_t data);
static uint32_t rd(uintptr_t base, uint32_t reg)
{
    return sd_test_mmio_read(base, reg);
}
static void wr(uintptr_t base, uint32_t reg, uint32_t data)
{
    sd_test_mmio_write(base, reg, data);
}
#else
static uint32_t rd(uintptr_t base, uint32_t reg)
{
    return *(volatile const uint32_t *)(base + reg);
}
static void wr(uintptr_t base, uint32_t reg, uint32_t data)
{
    *(volatile uint32_t *)(base + reg) = data;
}
#endif

uint8_t sd_crc7(const uint8_t *data, uint32_t length)
{
    uint8_t crc = 0;
    uint32_t i;
    while (length--) {
        uint8_t x = *data++;
        for (i = 0; i != 8; ++i) {
            uint8_t feedback = (uint8_t)(((crc >> 6) ^ (x >> 7)) & 1u);
            crc = (uint8_t)((crc << 1) & 0x7fu);
            if (feedback) crc ^= 0x09u;
            x = (uint8_t)(x << 1);
        }
    }
    return (uint8_t)((crc << 1) | 1u);
}

static uint16_t crc16_step(uint16_t crc, uint8_t value)
{
    uint32_t bit;
    crc ^= (uint16_t)value << 8;
    for (bit = 0; bit != 8; ++bit) {
        uint32_t next = (uint32_t)crc << 1;
        if ((crc & 0x8000u) != 0u) next ^= 0x1021u;
        crc = (uint16_t)next;
    }
    return crc;
}
uint16_t sd_crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc = 0;
    while (length--) crc = crc16_step(crc, *data++);
    return crc;
}

static int state_check(uint32_t status)
{
    if (!(status & PRESENT) || (status & 0x20u)) return SD_E_NO_CARD;
    if (status & 0x08u) return SD_E_TIMEOUT;
    if (status & 0x80u) return SD_E_BUS;
    return SD_OK;
}

/* Every wait has an explicit caller-supplied bound; no platform timer needed. */
static int wait_rx(uintptr_t base, uint32_t target, uint32_t timeout)
{
    uint32_t status;
    while (timeout--) {
        status = rd(base, STATUS);
        { int err = state_check(status); if (err) return err; }
        if (!(status & BUSY) && rd(base, RXCOUNT) >= target) return SD_OK;
    }
    return SD_E_TIMEOUT;
}
static int transfer(uintptr_t base, uint8_t tx, uint8_t *rx, uint32_t timeout)
{
    int rc;
    rc = state_check(rd(base, STATUS));
    if (rc) return rc;
    wr(base, TXBYTE, tx);
    rc = wait_rx(base, 1, timeout);
    if (rc) return rc;
    *rx = (uint8_t)rd(base, RXBYTE);
    return SD_OK;
}
static int trailing_clocks(uintptr_t base, uint32_t timeout)
{
    uint8_t ignored;
    int rc;
    wr(base, CTRL, 0); /* SD requires the trailing 8 clocks with CS high. */
    rc = transfer(base, 0xffu, &ignored, timeout);
    return rc;
}
static int cmd(uintptr_t base, uint8_t index, uint32_t arg, uint8_t *r1, uint32_t timeout)
{
    uint8_t packet[6], byte;
    uint32_t i;
    int rc;
    packet[0] = (uint8_t)(0x40u | index);
    packet[1] = (uint8_t)(arg >> 24);
    packet[2] = (uint8_t)(arg >> 16);
    packet[3] = (uint8_t)(arg >> 8);
    packet[4] = (uint8_t)arg;
    packet[5] = sd_crc7(packet, 5);
    wr(base, CTRL, 1); /* CS low across command, response and optional data. */
    for (i = 0; i != 6; ++i) {
        rc = transfer(base, packet[i], &byte, timeout);
        if (rc) return rc;
    }
    for (i = 0; i != 32; ++i) {
        rc = transfer(base, 0xff, &byte, timeout);
        if (rc) return rc;
        if ((byte & 0x80u) == 0) { *r1 = byte; return SD_OK; }
    }
    return SD_E_TIMEOUT;
}
static int command_end(uintptr_t base, uint32_t timeout)
{
    return trailing_clocks(base, timeout);
}
static int command_simple(uintptr_t base, uint8_t cmd_index, uint32_t arg,
                          uint8_t *r1, uint32_t timeout)
{
    int rc = cmd(base, cmd_index, arg, r1, timeout);
    if (rc) return rc;
    return command_end(base, timeout);
}
static int command_ocr(uintptr_t base, uint8_t index, uint32_t arg,
                       uint8_t *r1, uint32_t *ocr, uint32_t timeout)
{
    uint32_t i, word = 0;
    uint8_t b;
    int rc = cmd(base, index, arg, r1, timeout);
    if (rc) return rc;
    for (i = 0; i != 4; ++i) {
        rc = transfer(base, 0xff, &b, timeout);
        if (rc) return rc;
        word = (word << 8) | b;
    }
    *ocr = word;
    return command_end(base, timeout);
}

int sd_card_init(uintptr_t base, uint32_t timeout)
{
    uint32_t i, r7 = 0, ocr = 0;
    uint8_t r1, byte, v2 = 0, legacy_cmd1 = 0;
    int rc;
    initialized = 0; initialized_base = 0; high_capacity = 0;
    if (!base || !timeout || (base & 3u)) return SD_E_ARG;
    if (rd(base, ID) != 0x53445049u || (rd(base, CAP) & 3u) != 3u) return SD_E_BUS;
    wr(base, CTRL, 4); /* abort previous command and clear sticky state */
    if (!(rd(base, STATUS) & PRESENT)) return SD_E_NO_CARD;
    wr(base, DIVIDER, SD_SPI_INIT_DIVIDER);
    wr(base, TIMEOUT, timeout); /* hardware bound per byte in fabric-clock cycles */
    for (i = 0; i != 10; ++i) { /* >=80 clocks CS high / MOSI high */
        rc = transfer(base, 0xff, &byte, timeout);
        if (rc) goto fail;
    }
    for (i = 0; i != 16; ++i) {
        rc = command_simple(base, 0, 0, &r1, timeout);
        if (rc) goto fail;
        if (r1 == 1) break;
    }
    if (i == 16) { rc = SD_E_R1; goto fail; }
    rc = command_ocr(base, 8, 0x1aau, &r1, &r7, timeout);
    if (rc) goto fail;
    if (r1 == 1) {
        if ((r7 & 0xfffu) != 0x1aau) { rc = SD_E_R1; goto fail; }
        v2 = 1;
    } else if (r1 == 5) {
        v2 = 0; /* CMD8 illegal: older SDSC */
    } else { rc = SD_E_R1; goto fail; }

    for (i = 0; i != 1024; ++i) {
        if (!legacy_cmd1) {
            rc = command_simple(base, 55, 0, &r1, timeout);
            if (rc) goto fail;
            if (r1 & 4u) {
                if (v2) { rc = SD_E_R1; goto fail; }
                legacy_cmd1 = 1;
            } else if (r1 != 0 && r1 != 1) { rc = SD_E_R1; goto fail; }
        }
        rc = command_simple(base, legacy_cmd1 ? 1 : 41,
                            v2 ? 0x40000000u : 0u, &r1, timeout);
        if (rc) goto fail;
        if (r1 == 0) break;
        if (r1 != 1) {
            if (!v2 && (r1 & 4u)) legacy_cmd1 = 1;
            else { rc = SD_E_R1; goto fail; }
        }
    }
    if (i == 1024) { rc = SD_E_TIMEOUT; goto fail; }
    rc = command_ocr(base, 58, 0, &r1, &ocr, timeout);
    if (rc) goto fail;
    if (r1 != 0 || !(ocr & 0x80000000u)) { rc = SD_E_R1; goto fail; }
    high_capacity = (uint8_t)((ocr >> 30) & 1u);
    if (high_capacity && !v2) { rc = SD_E_R1; goto fail; }
    if (!high_capacity) {
        rc = command_simple(base, 16, 512, &r1, timeout);
        if (rc) goto fail;
        if (r1 != 0) { rc = SD_E_R1; goto fail; }
    }
    wr(base, DIVIDER, SD_SPI_DATA_DIVIDER);
    initialized_base = base; initialized = 1;
    return SD_OK;
fail:
    wr(base, CTRL, 4);
    return rc;
}

int sd_card_read_blocks(uintptr_t base, uint32_t lba, uint32_t count,
                        void *destination, uint32_t timeout)
{
    uint8_t *dst = (uint8_t *)destination;
    uint32_t sector, i, j, word, addr;
    uint16_t crc, card_crc;
    uint8_t r1, byte, msb;
    int rc;
    if (!initialized || base != initialized_base || !base ||
        (!destination && count) || !timeout) return SD_E_ARG;
    if (!count) return SD_OK;
    if (lba > UINT32_MAX - (count - 1u) || count > UINT32_MAX / 512u ||
        (!high_capacity && lba + count - 1u > UINT32_MAX / 512u)) return SD_E_CAPACITY;
    for (sector = 0; sector != count; ++sector) {
        addr = high_capacity ? lba + sector : (lba + sector) * 512u;
        rc = cmd(base, 17, addr, &r1, timeout);
        if (rc) goto fail;
        if (r1 != 0) { rc = SD_E_R1; goto fail; }
        for (i = 0; i != timeout; ++i) {
            rc = transfer(base, 0xff, &byte, timeout);
            if (rc) goto fail;
            if (byte == 0xfe) break;
            if (byte != 0xff) { rc = SD_E_TOKEN; goto fail; }
        }
        if (i == timeout) { rc = SD_E_TIMEOUT; goto fail; }
        wr(base, RXBURST, 512);
        rc = wait_rx(base, 512, timeout);
        if (rc) goto fail;
        crc = 0;
        for (i = 0; i != 128; ++i) {
            word = rd(base, RXWORD);
            for (j = 0; j != 4; ++j) {
                byte = (uint8_t)(word >> (j * 8u));
                crc = crc16_step(crc, byte);
                dst[sector * 512u + i * 4u + j] = byte;
            }
        }
        rc = transfer(base, 0xff, &msb, timeout);
        if (rc) goto fail;
        rc = transfer(base, 0xff, &byte, timeout);
        if (rc) goto fail;
        card_crc = (uint16_t)(((uint16_t)msb << 8) | byte);
        if (card_crc != crc) { rc = SD_E_CRC; goto fail; }
        rc = command_end(base, timeout);
        if (rc) goto fail;
    }
    return SD_OK;
fail:
    wr(base, CTRL, 4); /* release CS and FIFO on any CRC, timeout or removal */
    if (rc == SD_E_NO_CARD) initialized = 0;
    return rc;
}
