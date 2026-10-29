#include "platform.h"
#ifndef CPU_HZ
#define CPU_HZ 102000000u
#endif
#define DDR_STATUS 0x10000200u
static uint32_t pattern(uint32_t i) { return (i * 0x9e3779b9u) ^ 0x5aa59669u; }
static int fail(uint32_t addr, uint32_t got, uint32_t expected) {
    puts_uart("DDR FAIL addr="); puthex(addr); puts_uart(" got="); puthex(got);
    puts_uart(" expected="); puthex(expected); puts_uart("\r\n"); return 1;
}
int main(void) {
    puts_uart("AG DDR3 smoke CPU="); puthex(CPU_HZ); puts_uart("\r\n");
    uint64_t start = read_cycles();
    while (!(MMIO32(DDR_STATUS) & 1)) {
        if ((MMIO32(DDR_STATUS) & 2) || read_cycles() - start > (uint64_t)CPU_HZ * 5) {
            puts_uart("DDR INIT FAIL status="); puthex(MMIO32(DDR_STATUS)); puts_uart("\r\n"); return 2;
        }
    }
    puts_uart("DDR calibrated\r\n");
    // 16 KiB includes all byte lanes, burst boundaries and multiple columns.
    volatile uint32_t *ram = (volatile uint32_t *)0x80000000u;
    for (uint32_t i = 0; i < 4096; ++i) ram[i] = pattern(i);
    for (uint32_t i = 0; i < 4096; ++i) {
        uint32_t got = ram[i];
        if (got != pattern(i)) return fail(0x80000000u + 4*i, got, pattern(i));
    }
    volatile unsigned char *bytes = (volatile unsigned char *)0x80004000u;
    for (uint32_t i = 0; i < 64; ++i) bytes[i] = (unsigned char)(i ^ 0xa5);
    for (uint32_t i = 0; i < 64; ++i) if (bytes[i] != (i ^ 0xa5)) return fail(0x80004000u+i, bytes[i], i ^ 0xa5);
    volatile unsigned short *halves = (volatile unsigned short *)0x80004040u;
    for (uint32_t i = 0; i < 32; ++i) halves[i] = (unsigned short)(i ^ 0x5aa5);
    for (uint32_t i = 0; i < 32; ++i) if (halves[i] != (i ^ 0x5aa5)) return fail(0x80004040u+2*i, halves[i], i ^ 0x5aa5);
    // Address-line alias check across all 1 GiB; preserve a distinct base value.
    ram[0] = 0x12345678;
    for (uint32_t bit = 2; bit < 30; ++bit) MMIO32(0x80000000u + (1u << bit)) = pattern(bit);
    if (ram[0] != 0x12345678) return fail(0x80000000u, ram[0], 0x12345678);
    for (uint32_t bit = 2; bit < 30; ++bit) {
        uint32_t addr = 0x80000000u + (1u << bit), got = MMIO32(addr);
        if (got != pattern(bit)) return fail(addr, got, pattern(bit));
    }
    MMIO32(0xbffffffcu) = 0xaabbccdd;
    if (MMIO32(0xbffffffcu) != 0xaabbccdd) return fail(0xbffffffcu, MMIO32(0xbffffffcu), 0xaabbccdd);
    // addi a0,zero,42; ret. Entry is the last word of a 32-byte burst.
    MMIO32(0x8000801cu) = 0x02a00513;
    MMIO32(0x80008020u) = 0x00008067;
    __asm__ volatile("fence.i" ::: "memory");
    int got = ((int (*)(void))0x8000801cu)();
    if (got != 42) return fail(0x8000801cu, (uint32_t)got, 42);
    puts_uart("DDR PASS data16KiB byte halfword address-lines edge execute\r\n");
    return 0;
}
