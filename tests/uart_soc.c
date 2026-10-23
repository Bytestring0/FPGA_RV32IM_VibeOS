#include <stdint.h>
static volatile uint32_t scratch[6];
int main(void) {
    const uint8_t bytes[] = {0x00, 0xff, 0xa5, 0x5a, 0x81, 0x7e};
    for (unsigned i = 0; i < 6; ++i) {
        // Deliberately do not poll TX ready: MMIO must apply backpressure.
        *(volatile uint32_t *)0x10000000 = bytes[i];
        scratch[i] = i * 37 + 9;
    }
    for (unsigned i = 0; i < 6; ++i)
        if (scratch[i] != i * 37 + 9) return 1;
    return 0;
}
