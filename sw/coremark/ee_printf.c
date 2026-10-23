// Integer-only freestanding console formatter; outside benchmark timed region.
#include <stdarg.h>
#include "../platform.h"
static int print_uint(uint32_t n, unsigned base, unsigned width, char pad) {
    char buffer[32]; unsigned len = 0; int count = 0;
    do { buffer[len++] = "0123456789abcdef"[n % base]; n /= base; } while (n);
    while (width > len) { putchar_uart(pad); --width; ++count; }
    while (len) { putchar_uart(buffer[--len]); ++count; }
    return count;
}
int ee_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int count = 0;
    while (*fmt) {
        if (*fmt != '%') { putchar_uart(*fmt++); ++count; continue; }
        ++fmt; char pad = ' '; unsigned width = 0;
        if (*fmt == '0') { pad = '0'; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (unsigned)(*fmt++ - '0');
        if (*fmt == 'l') ++fmt; // RV32 long == int.
        switch (*fmt++) {
            case 's': { const char *s = va_arg(ap, const char *);
                while (*s) { putchar_uart(*s++); ++count; } break; }
            case 'd': { int v = va_arg(ap, int);
                if (v < 0) { putchar_uart('-'); ++count; }
                count += print_uint(v < 0 ? 0u-(uint32_t)v : (uint32_t)v, 10, width, pad); break; }
            case 'u': count += print_uint(va_arg(ap, unsigned), 10, width, pad); break;
            case 'x': count += print_uint(va_arg(ap, unsigned), 16, width, pad); break;
            case 'c': putchar_uart((char)va_arg(ap, int)); ++count; break;
            case '%': putchar_uart('%'); ++count; break;
            default: putchar_uart('?'); ++count; break;
        }
    }
    va_end(ap); return count;
}
