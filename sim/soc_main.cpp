#include "Vag_soc.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Vag_soc top;
    uint64_t limit = 200000000;
    if (const char *v = std::getenv("MAX_CYCLES")) limit = std::strtoull(v, nullptr, 0);
    top.clk = 0; top.rst = 1; top.irq_external = 0;
    for (int i = 0; i < 5; ++i) {
        top.clk = 0; top.eval(); top.clk = 1; top.eval();
    }
    top.rst = 0;
    top.irq_external = std::getenv("IRQ_EXTERNAL") != nullptr;
    for (uint64_t cycle = 0; cycle < limit && !Verilated::gotFinish(); ++cycle) {
        top.clk = 0; top.eval();
        top.clk = 1; top.eval();
        if (top.console_valid) { std::putchar(top.console_data); std::fflush(stdout); }
        if (top.done) {
            std::printf("\nSIM exit=%u cycles=%llu instret=%llu branches=%u misses=%u "
                        "load_stalls=%u dual_retire_count=%u\n",
                top.exit_code, (unsigned long long)top.cycle_count,
                (unsigned long long)top.instret_count,
                top.branch_count, top.mispredict_count, top.load_stall_count,
                top.dual_retire_count);
            return top.exit_code ? 1 : 0;
        }
    }
    std::fprintf(stderr, "TIMEOUT pc=%08x trap=%u cause=%u\n", top.retire_pc, top.trap_valid, top.trap_cause);
    return 2;
}
