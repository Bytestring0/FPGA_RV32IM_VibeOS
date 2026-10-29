#include "platform.h"
static volatile uint32_t memory[16];
int main(void) {
    uint32_t sum = 0, x = 0x12345678;
    for (uint32_t i = 0; i < 16; ++i) memory[i] = i * 37 + 11;
    for (uint32_t i = 0; i < 16; ++i) sum += memory[i];
    if (sum != 4616) return 1;
    for (uint32_t i = 1; i < 100; ++i) x = (x ^ (x >> 7)) * 1664525u + 1013904223u;
    puts_uart("AG RV32IM boot OK, hash=");
    puthex(x);
    puts_uart("\n");
    return 0;
}
