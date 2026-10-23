#ifndef QINGLONG_PLATFORM_H
#define QINGLONG_PLATFORM_H
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#define MMIO32(addr) (*(volatile uint32_t *)(addr))
#define UART_DATA 0x10000000u
#define MTIME_LO  0x10000100u
#define MTIME_HI  0x10000104u
#define MTIMECMP_LO 0x10000108u
#define MTIMECMP_HI 0x1000010cu
static inline uint64_t read_cycles(void) {
    uint32_t hi, lo, again;
    do {
        __asm__ volatile("rdcycleh %0" : "=r"(hi) : : "memory");
        __asm__ volatile("rdcycle %0" : "=r"(lo) : : "memory");
        __asm__ volatile("rdcycleh %0" : "=r"(again) : : "memory");
    } while (hi != again);
    return ((uint64_t)hi << 32) | lo;
}
static inline void putchar_uart(char c) { MMIO32(UART_DATA) = (unsigned char)c; }
static inline void puts_uart(const char *s) { while (*s) putchar_uart(*s++); }
static inline void puthex(uint32_t value) {
    const char *digits = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4) putchar_uart(digits[(value >> i) & 15]);
}
#endif
