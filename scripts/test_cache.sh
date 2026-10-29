#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/cache-tests
word_only="${LOAD_WORD_ONLY:-0}"
set_bits="${CACHE_SET_BITS:-8}"
make cache-firmware
verilator --cc --exe --build -j 4 -O3 -Wno-fatal --top-module ag_l1cache -GSET_BITS=2 rtl/ag_l1cache.sv --Mdir build/cache-tests/unit "$PWD/sim/cache_main.cpp" > build/cache-tests/unit-build.log 2>&1
build/cache-tests/unit/Vag_l1cache | tee build/cache-tests/unit.log
for mode in 1 0; do
  out="build/cache-tests/mode${mode}"
  verilator --cc --exe --build -j 4 -O3 -Wno-fatal --top-module ag_soc -f rtl/files.f -GLOAD_WORD_ONLY="$word_only" -GCACHE_SET_BITS="$set_bits" -GDDR_ENABLE=1 -GCACHE_ENABLE="$mode" -GSIM_FAST_UART=1 -GMULH_PIPELINE=1 '-GINIT_FILE="build/cache_smoke.hex"' --Mdir "$out" "$PWD/sim/ddr_soc_main.cpp" > "build/cache-tests/mode${mode}-build.log" 2>&1
  "$out/Vag_soc" | tee "build/cache-tests/mode${mode}.log"
done
