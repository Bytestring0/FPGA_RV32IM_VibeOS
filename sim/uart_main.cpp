#include "Vuart_tx.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Vuart_tx tx;
    auto tick = [&]() { tx.clk = 0; tx.eval(); tx.clk = 1; tx.eval(); };
    auto check = [&](bool condition, const char *reason) {
        if (!condition) { std::fprintf(stderr, "UART FAIL: %s\n", reason); std::exit(1); }
    };
    tx.rst = 1; tx.valid = 0; tx.data = 0; tick();
    tx.rst = 0; tick();
    check(tx.ready && tx.tx, "reset must leave the line idle");
    constexpr unsigned divisor = 27000000 / 115200;
    const unsigned bytes[] = {0x00, 0xff, 0xa5, 0x5a, 0x81, 0x7e};
    for (unsigned byte : bytes) {
        tx.data = byte; tx.valid = 1; tick();
        // Keep valid asserted and corrupt the input during busy: no overwrite.
        tx.data = byte ^ 0xff;
        unsigned frame = (1u << 9) | (byte << 1);
        for (unsigned cycle = 0; cycle < 10 * divisor; ++cycle) {
            check(!tx.ready, "ready asserted before stop bit completed");
            check(tx.tx == ((frame >> (cycle / divisor)) & 1u), "wrong bit or bit duration");
            tick();
        }
        check(tx.ready && tx.tx, "stop bit must finish in idle");
    }
    tx.valid = 1; tx.data = 0; tick();
    tx.rst = 1; tick();
    check(tx.ready && tx.tx, "reset during a frame must return to idle");
    std::puts("UART PASS: six 8N1 frames, exact 234-cycle bits, busy backpressure, reset");
}
