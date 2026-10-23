#include "Vqinglong_soc.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Vqinglong_soc top;
    auto tick = [&]() { top.clk = 0; top.eval(); top.clk = 1; top.eval(); };
    auto check = [](bool condition, const char *reason) {
        if (!condition) { std::fprintf(stderr, "SoC UART FAIL: %s\n", reason); std::exit(1); }
    };
    top.rst = 1; top.irq_external = 0;
    for (int i = 0; i < 5; ++i) tick();
    top.rst = 0;
    const unsigned bytes[] = {0x00, 0xff, 0xa5, 0x5a, 0x81, 0x7e};
    constexpr unsigned divisor = 27000000 / 115200;
    unsigned count = 0, age = 0, frame = 0;
    bool active = false;
    for (unsigned cycle = 0; cycle < 100000; ++cycle) {
        tick();
        if (top.console_valid) {
            check(!active, "accepted another byte before previous frame ended");
            check(count < 6 && top.console_data == bytes[count], "lost, duplicated or reordered byte");
            frame = (1u << 9) | (bytes[count++] << 1); age = 0; active = true;
        }
        if (active) {
            check(top.uart_tx_o == ((frame >> (age / divisor)) & 1u), "incorrect serial bit or duration");
            if (++age == 10 * divisor) active = false;
        } else check(top.uart_tx_o, "unexpected start or non-idle line");
        if (top.done && count == 6 && !active) {
            check(top.exit_code == 0, "interleaved RAM access failed");
            std::puts("SoC UART PASS: six unpolled MMIO writes, exact 8N1 waveform, interleaved RAM");
            return 0;
        }
    }
    std::fputs("SoC UART FAIL: timeout\n", stderr); return 1;
}
