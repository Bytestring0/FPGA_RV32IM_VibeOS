#include "sd_card.h"

/* Opt in with -DNPU_FAST_CRC=1; retain the original implementation by default. */
#ifndef NPU_FAST_CRC
#define NPU_FAST_CRC 0
#endif
/* Optional SPI CMD18 stream. The default remains one CMD17 per sector. */
#ifndef SD_SPI_MULTIBLOCK
#define SD_SPI_MULTIBLOCK 0
#endif

/* Capability-gated acceleration; 0 forces the software CRC reference path. */
#ifndef SD_SPI_HW_CRC
#define SD_SPI_HW_CRC 1
#endif

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
#define RXCRC16   0x2cu
#define CAP_CRC16 0x04u
#define CRC_VALID 0x10000u
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
#if SD_SPI_HW_CRC == 1
static uint8_t hardware_crc;
#endif
#if SD_SPI_MULTIBLOCK == 1
static uint8_t multiblock_supported;
#endif
static sd_card_stats statistics;

void sd_card_get_stats(sd_card_stats *out)
{
    if (out) *out = statistics;
}

void sd_card_reset_stats(void)
{
    uint32_t mode = statistics.crc_mode;
    statistics = (sd_card_stats){0};
    statistics.crc_mode = mode;
}

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
#if NPU_FAST_CRC == 1
    /* 256 remainders: MSB first, polynomial 0x1021, no reflection or XOR. */
    static const uint16_t table[256] = {
        0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50a5u, 0x60c6u, 0x70e7u,
        0x8108u, 0x9129u, 0xa14au, 0xb16bu, 0xc18cu, 0xd1adu, 0xe1ceu, 0xf1efu,
        0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52b5u, 0x4294u, 0x72f7u, 0x62d6u,
        0x9339u, 0x8318u, 0xb37bu, 0xa35au, 0xd3bdu, 0xc39cu, 0xf3ffu, 0xe3deu,
        0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64e6u, 0x74c7u, 0x44a4u, 0x5485u,
        0xa56au, 0xb54bu, 0x8528u, 0x9509u, 0xe5eeu, 0xf5cfu, 0xc5acu, 0xd58du,
        0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76d7u, 0x66f6u, 0x5695u, 0x46b4u,
        0xb75bu, 0xa77au, 0x9719u, 0x8738u, 0xf7dfu, 0xe7feu, 0xd79du, 0xc7bcu,
        0x48c4u, 0x58e5u, 0x6886u, 0x78a7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
        0xc9ccu, 0xd9edu, 0xe98eu, 0xf9afu, 0x8948u, 0x9969u, 0xa90au, 0xb92bu,
        0x5af5u, 0x4ad4u, 0x7ab7u, 0x6a96u, 0x1a71u, 0x0a50u, 0x3a33u, 0x2a12u,
        0xdbfdu, 0xcbdcu, 0xfbbfu, 0xeb9eu, 0x9b79u, 0x8b58u, 0xbb3bu, 0xab1au,
        0x6ca6u, 0x7c87u, 0x4ce4u, 0x5cc5u, 0x2c22u, 0x3c03u, 0x0c60u, 0x1c41u,
        0xedaeu, 0xfd8fu, 0xcdecu, 0xddcdu, 0xad2au, 0xbd0bu, 0x8d68u, 0x9d49u,
        0x7e97u, 0x6eb6u, 0x5ed5u, 0x4ef4u, 0x3e13u, 0x2e32u, 0x1e51u, 0x0e70u,
        0xff9fu, 0xefbeu, 0xdfddu, 0xcffcu, 0xbf1bu, 0xaf3au, 0x9f59u, 0x8f78u,
        0x9188u, 0x81a9u, 0xb1cau, 0xa1ebu, 0xd10cu, 0xc12du, 0xf14eu, 0xe16fu,
        0x1080u, 0x00a1u, 0x30c2u, 0x20e3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
        0x83b9u, 0x9398u, 0xa3fbu, 0xb3dau, 0xc33du, 0xd31cu, 0xe37fu, 0xf35eu,
        0x02b1u, 0x1290u, 0x22f3u, 0x32d2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
        0xb5eau, 0xa5cbu, 0x95a8u, 0x8589u, 0xf56eu, 0xe54fu, 0xd52cu, 0xc50du,
        0x34e2u, 0x24c3u, 0x14a0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
        0xa7dbu, 0xb7fau, 0x8799u, 0x97b8u, 0xe75fu, 0xf77eu, 0xc71du, 0xd73cu,
        0x26d3u, 0x36f2u, 0x0691u, 0x16b0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
        0xd94cu, 0xc96du, 0xf90eu, 0xe92fu, 0x99c8u, 0x89e9u, 0xb98au, 0xa9abu,
        0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18c0u, 0x08e1u, 0x3882u, 0x28a3u,
        0xcb7du, 0xdb5cu, 0xeb3fu, 0xfb1eu, 0x8bf9u, 0x9bd8u, 0xabbbu, 0xbb9au,
        0x4a75u, 0x5a54u, 0x6a37u, 0x7a16u, 0x0af1u, 0x1ad0u, 0x2ab3u, 0x3a92u,
        0xfd2eu, 0xed0fu, 0xdd6cu, 0xcd4du, 0xbdaau, 0xad8bu, 0x9de8u, 0x8dc9u,
        0x7c26u, 0x6c07u, 0x5c64u, 0x4c45u, 0x3ca2u, 0x2c83u, 0x1ce0u, 0x0cc1u,
        0xef1fu, 0xff3eu, 0xcf5du, 0xdf7cu, 0xaf9bu, 0xbfbau, 0x8fd9u, 0x9ff8u,
        0x6e17u, 0x7e36u, 0x4e55u, 0x5e74u, 0x2e93u, 0x3eb2u, 0x0ed1u, 0x1ef0u,
    };
    return (uint16_t)(((uint32_t)crc << 8) ^ table[((crc >> 8) ^ value) & 0xffu]);
#else
    uint32_t bit;
    crc ^= (uint16_t)value << 8;
    for (bit = 0; bit != 8; ++bit) {
        uint32_t next = (uint32_t)crc << 1;
        if ((crc & 0x8000u) != 0u) next ^= 0x1021u;
        crc = (uint16_t)next;
    }
    return crc;
#endif
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
    if (index == 17) ++statistics.cmd17;
    if (index == 18) ++statistics.cmd18;
    if (index == 12) ++statistics.cmd12;
    wr(base, CTRL, 1); /* CS low across command, response and optional data. */
    for (i = 0; i != 6; ++i) {
        rc = transfer(base, packet[i], &byte, timeout);
        if (rc) return rc;
    }
#if SD_SPI_MULTIBLOCK == 1
    if (index == 12) {
        /* SPI STOP_TRANSMISSION has one stuff byte before its R1b response.
         * It may look like a valid R1, so discard it unconditionally. */
        rc = transfer(base, 0xff, &byte, timeout);
        if (rc) return rc;
    }
#endif
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

#if SD_SPI_MULTIBLOCK == 1
static int stop_read(uintptr_t base, uint32_t timeout)
{
    uint8_t r1, byte;
    uint32_t i;
    /* A software RX deadline may expire while the autonomous 512-byte burst
     * is still running. Never submit TXBYTE into that active transaction. */
    int rc = wait_rx(base, 0, timeout);
    if (rc) return rc;
    for (i = 0; i < 512u && rd(base, RXCOUNT); ++i) (void)rd(base, RXBYTE);
    if (rd(base, RXCOUNT)) return SD_E_BUS;
    rc = cmd(base, 12, 0, &r1, timeout);
    if (rc) return rc;
    if (r1 != 0) return SD_E_R1;
    /* R1b busy holds MISO low. Bound both the byte transaction and number
     * of busy bytes; no new command or CS release before ready. */
    for (i = 0; i != timeout; ++i) {
        rc = transfer(base, 0xff, &byte, timeout);
        if (rc) return rc;
        if (byte == 0xff) return command_end(base, timeout);
    }
    return SD_E_TIMEOUT;
}
#endif

int sd_card_init(uintptr_t base, uint32_t timeout)
{
    uint32_t i, r7 = 0, ocr = 0;
    uint8_t r1, byte, v2 = 0, legacy_cmd1 = 0;
    int rc;
    initialized = 0; initialized_base = 0; high_capacity = 0;
    statistics.crc_mode = 0;
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
#if SD_SPI_HW_CRC == 1
    hardware_crc = (uint8_t)((rd(base, CAP) & CAP_CRC16) != 0);
#endif
#if SD_SPI_MULTIBLOCK == 1
    multiblock_supported = 1;
#endif
    statistics.crc_mode = 1;
#if SD_SPI_HW_CRC == 1
    if (hardware_crc) statistics.crc_mode = 2;
#endif
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
#if SD_SPI_MULTIBLOCK == 1
    uint8_t multiple = count >= 2 && multiblock_supported, stream_started = 0;
#endif
    ++statistics.read_calls;
    if (!initialized || base != initialized_base || !base ||
        (!destination && count) || !timeout) {
        ++statistics.errors;
        return SD_E_ARG;
    }
    if (!count) return SD_OK;
    if (lba > UINT32_MAX - (count - 1u) || count > UINT32_MAX / 512u ||
        (!high_capacity && lba + count - 1u > UINT32_MAX / 512u)) {
        ++statistics.errors;
        return SD_E_CAPACITY;
    }
    statistics.blocks_requested += count;
#if SD_SPI_MULTIBLOCK == 1
    if (multiple) {
        addr = high_capacity ? lba : lba * 512u;
        /* A failed response can leave command acceptance uncertain. Attempt
         * CMD12 on all errors after issuing CMD18, including response errors. */
        stream_started = 1;
        rc = cmd(base, 18, addr, &r1, timeout);
        if (rc) goto fail;
        if (r1 == 4u) {
            /* Explicit ILLEGAL_COMMAND means no stream was accepted. End the
             * command normally and retain CMD17 fallback until next init.
             * Other R1, CRC and transport faults must never be retried as data. */
            stream_started = 0; /* An explicit reject did not start a stream. */
            for (i = 0; i != timeout; ++i) {
                rc = transfer(base, 0xff, &byte, timeout);
                if (rc) { initialized = 0; goto fail; }
                if (byte == 0xff) break;
            }
            if (i == timeout) { initialized = 0; rc = SD_E_TIMEOUT; goto fail; }
            rc = command_end(base, timeout);
            if (rc) { initialized = 0; goto fail; }
            multiple = 0; multiblock_supported = 0;
            ++statistics.cmd18_fallback;
        } else if (r1 != 0) { rc = SD_E_R1; goto fail; }
    }
#endif
    for (sector = 0; sector != count; ++sector) {
#if SD_SPI_MULTIBLOCK == 1
        if (!multiple) {
#endif
        addr = high_capacity ? lba + sector : (lba + sector) * 512u;
        rc = cmd(base, 17, addr, &r1, timeout);
        if (rc) goto fail;
        if (r1 != 0) { rc = SD_E_R1; goto fail; }
#if SD_SPI_MULTIBLOCK == 1
        }
#endif
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
#if SD_SPI_HW_CRC == 1
        if (hardware_crc) {
            ++statistics.crc_hardware;
            word = rd(base, RXCRC16);
            if (!(word & CRC_VALID)) { rc = SD_E_BUS; goto fail; }
            crc = (uint16_t)word;
        }
#endif
#if SD_SPI_HW_CRC == 1
        if (!hardware_crc)
#endif
            ++statistics.crc_software;
        for (i = 0; i != 128; ++i) {
            word = rd(base, RXWORD);
#if SD_SPI_HW_CRC == 1
            if (hardware_crc) {
                uint8_t *out = dst + sector * 512u + i * 4u;
                /* Byte stores preserve arbitrary alignment and C alias rules. */
                out[0] = (uint8_t)word;
                out[1] = (uint8_t)(word >> 8);
                out[2] = (uint8_t)(word >> 16);
                out[3] = (uint8_t)(word >> 24);
                continue;
            }
#endif
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
        ++statistics.crc_verified;
#if SD_SPI_MULTIBLOCK == 1
        if (!multiple) {
#endif
        rc = command_end(base, timeout);
        if (rc) goto fail;
#if SD_SPI_MULTIBLOCK == 1
        }
#endif
    }
#if SD_SPI_MULTIBLOCK == 1
    if (multiple) {
        stream_started = 0; /* A failed explicit stop already attempted recovery. */
        rc = stop_read(base, timeout);
        if (rc) { initialized = 0; goto fail; }
    }
#endif
    statistics.blocks_completed += count;
    return SD_OK;
fail:
    ++statistics.errors;
#if SD_SPI_MULTIBLOCK == 1
    if (stream_started && stop_read(base, timeout) != SD_OK) initialized = 0;
#endif
    wr(base, CTRL, 4); /* release CS and FIFO on any CRC, timeout or removal */
    if (rc == SD_E_NO_CARD) initialized = 0;
    if (!initialized) statistics.crc_mode = 0;
    return rc;
}
