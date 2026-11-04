// Actual-shape weighted isolated fused RTL budget; synthetic numeric data.
#define main fused_regression_main
#include "test_npu_w4_fused.cpp"
#undef main
#include <fstream>
#include <sstream>
#include <iostream>

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    try {
        if (argc != 2) throw std::runtime_error("expected n,k,calls,head_calls CSV");
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("missing shape census");
        std::string line; std::getline(input, line);
        uint64_t total = 0; unsigned shapes = 0;
        while (std::getline(input, line)) {
            for (char &c : line) if (c == ',') c = ' ';
            std::istringstream row(line); unsigned n,k,calls,head_calls;
            if (!(row >> n >> k >> calls >> head_calls) || !n || n > 4096 || !k || k > 4096 || !calls || head_calls > calls)
                throw std::runtime_error("invalid real matrix shape");
            // Fresh BFM state gives each measured shape a reproducible request phase.
            Tb b; b.mem.resize(16u << 20, 0);
            b.run_fused(n, k, 2147483647u, 17u);
            uint64_t cycles = b.d.cycle_count;
            uint64_t weighted = cycles * calls; total += weighted; ++shapes;
            std::cout << "MATRIX_BUDGET," << n << "," << k << "," << calls << "," << head_calls << "," << cycles << "," << b.reads << "," << b.writes << "," << weighted << std::endl;
        }
        if (!shapes) throw std::runtime_error("empty shape census");
        std::cout << "FUSED_MATRIX_BUDGET_PASS shapes=" << shapes << " weighted_cycles=" << total
                  << " synthetic_operands=true board_verified=false mmio_excluded=true" << std::endl;
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
