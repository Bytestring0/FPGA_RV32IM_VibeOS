/* Standalone host check; no board or model required.
 * From system/: cc -std=c99 -O2 -Wall -Wextra -Werror -DNPU_FAST_CRC=1
 *                   sim/test_crc.c -o /tmp/npu_crc && /tmp/npu_crc
 * Omit NPU_FAST_CRC, or set it to 0, to check the original implementation.
 * Add -DCRC_CHECK_VENDOR=1 to check the self-contained runtime vendor copy.
 * Including the sources also checks the internal byte-update contracts without
 * adding an exported firmware API. No SD/MMIO routine is called by this test.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(CRC_CHECK_VENDOR) && CRC_CHECK_VENDOR
#include "../integration/agent_runtime_20261101/sw/model/vendor/npu_v1/sd_card.c"
#include "../integration/agent_runtime_20261101/sw/model/vendor/npu_v1/npu_package.c"
#else
#include "../sw/sd_card.c"
#include "../sw/npu_package.c"
#endif

#define CHECK(x) do { if (!(x)) { \
    printf("FAIL_CRC line=%d fast=%d\n", __LINE__, NPU_FAST_CRC); return 1; \
} } while (0)

static uint16_t reference16(uint16_t crc, uint8_t value)
{
    unsigned bit;
    crc ^= (uint16_t)value << 8;
    for (bit = 0; bit != 8; ++bit) {
        uint32_t next = (uint32_t)crc << 1;
        if (crc & 0x8000u) next ^= 0x1021u;
        crc = (uint16_t)next;
    }
    return crc;
}

static uint32_t reference32(uint32_t crc, uint8_t value)
{
    unsigned bit;
    crc ^= value;
    for (bit = 0; bit != 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    return crc;
}

static uint32_t random_state = UINT32_C(0x61c88647);
static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

int main(void)
{
    static const uint8_t vector[] = "123456789";
    static const uint32_t lengths[] = {0, 1, 7, 8, 9, 47, 48, 49, 50, 51,
                                      52, 53, 255, 256, 511, 512, 513, 4096};
    uint8_t data[4096], zeroed[4096];
    uint32_t state, value, trial, offset, length, i;
    uint64_t partial_bytes = 0;

    CHECK(sd_crc16(vector, 9) == 0x31c3u);
    CHECK(npu_package_crc32(vector, 9) == UINT32_C(0xcbf43926));
    CHECK(sd_crc16(NULL, 0) == 0u);
    CHECK(npu_package_crc32(NULL, 0) == 0u);

    /* Every CRC16 state and input byte, including nonzero partial CRCs. */
    for (state = 0; state != 65536u; ++state)
        for (value = 0; value != 256u; ++value)
            CHECK(crc16_step((uint16_t)state, (uint8_t)value) ==
                  reference16((uint16_t)state, (uint8_t)value));

    /* Every CRC32 table index, mixed with arbitrary previous CRC states. */
    for (trial = 0; trial != 4096u; ++trial) {
        state = next_random();
        for (value = 0; value != 256u; ++value)
            CHECK(crc_byte(state, (uint8_t)value) ==
                  reference32(state, (uint8_t)value));
    }

    for (trial = 0; trial != 1042u; ++trial) {
        uint16_t r16 = 0, c16 = 0;
        uint32_t r32 = UINT32_C(0xffffffff), c32 = r32;
        length = trial < sizeof(lengths) / sizeof(lengths[0]) ? lengths[trial] :
                 next_random() % (sizeof(data) + 1u);
        for (i = 0; i != length; ++i) data[i] = (uint8_t)next_random();
        /* Random partial updates; compare every chunk with bit-serial reference. */
        for (offset = 0; offset != length;) {
            uint32_t end = offset + 1u + (next_random() % 137u);
            if (end > length) end = length;
            for (; offset != end; ++offset) {
                r16 = reference16(r16, data[offset]);
                r32 = reference32(r32, data[offset]);
                c16 = crc16_step(c16, data[offset]);
                c32 = crc_byte(c32, data[offset]);
                ++partial_bytes;
            }
            CHECK(c16 == r16 && c32 == r32);
        }
        CHECK(sd_crc16(data, length) == r16);
        CHECK(npu_package_crc32(data, length) == (r32 ^ UINT32_C(0xffffffff)));

        /* Header CRC field [48,52) is zero only during the image CRC. */
        memcpy(zeroed, data, length);
        for (i = 48u; i < 52u && i < length; ++i) zeroed[i] = 0;
        r32 = UINT32_C(0xffffffff);
        for (i = 0; i != length; ++i) r32 = reference32(r32, zeroed[i]);
        r32 ^= UINT32_C(0xffffffff);
        CHECK(image_crc(data, length) == r32);
        CHECK(npu_package_crc32(zeroed, length) == r32);
        for (i = 48u; i < 52u && i < length; ++i) data[i] ^= 0xffu;
        CHECK(image_crc(data, length) == r32);
        if (length > 52u) {
            data[52] ^= 1u;
            CHECK(image_crc(data, length) != r32);
        }
    }
    printf("PASS_CRC fast=%d crc16=31c3 crc32=cbf43926 "
           "crc16_steps=16777216 crc32_steps=1048576 buffers=1042 "
           "partial_bytes=%llu header_zero=48..51\n",
           NPU_FAST_CRC, (unsigned long long)partial_bytes);
    return 0;
}
