#!/usr/bin/env bash
set -euo pipefail
cd /mnt/d/FPGA/RiscV
out=build/ag-rename-validation
run_test() {
  local name="$1"; shift
  echo "START $name: $*"
  if "$@" > "$out/$name.log" 2>&1; then
    echo "PASS $name"
    tail -n 6 "$out/$name.log"
  else
    local status=$?
    echo "FAIL $name ($status)"
    tail -n 80 "$out/$name.log"
    exit "$status"
  fi
}
run_test lint make lint
run_test check make check
run_test ddr bash scripts/test_ddr.sh
run_test cache make cache-test
run_test coremark make coremark-opt CPU_HZ=102000000 MULH_PIPELINE=1
run_test cache-firmware make cache-firmware
run_test smoke make smoke
printf 'ALL AG RENAME REGRESSIONS PASSED\n'
