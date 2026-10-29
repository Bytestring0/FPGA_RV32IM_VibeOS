#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/ddr-tests
verilator --binary --timing --assert -j 4 -Wno-fatal --top-module ddr_bridge_tb \
  --Mdir build/ddr-tests/bridge rtl/ag_extmem.sv rtl/ag_hmic_bridge.sv \
  sim/ddr_bridge_tb.sv > build/ddr-tests/compile.log 2>&1
for half in 4.901961 3.1 7.3; do
  build/ddr-tests/bridge/Vddr_bridge_tb +CPU_HALF="$half"
done
make ddr-firmware
verilator --cc --exe --build -j 4 -O3 -Wno-fatal --top-module ag_soc \
  -f rtl/files.f "-GDDR_ENABLE=1'b1" "-GSIM_FAST_UART=1'b1" "-GMULH_PIPELINE=1'b1" \
  '-GINIT_FILE="build/ddr_smoke.hex"' --Mdir build/ddr-tests/soc \
  "$(pwd)/sim/ddr_soc_main.cpp" > build/ddr-tests/soc-compile.log 2>&1
build/ddr-tests/soc/Vag_soc
