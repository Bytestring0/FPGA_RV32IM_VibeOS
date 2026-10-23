#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party
test -d third_party/coremark/.git || git clone https://github.com/eembc/coremark.git third_party/coremark
test -d third_party/riscv-tests/.git || git clone https://github.com/riscv-software-src/riscv-tests.git third_party/riscv-tests
git -C third_party/coremark checkout --detach 1f483d5b8316753a742cbf5590caf5bd0a4e4777
git -C third_party/riscv-tests checkout --detach 3df703cd52d5f0d8224e3c50bf03c6cb70efff98
printf 'CoreMark: '
git -C third_party/coremark rev-parse HEAD
printf 'riscv-tests: '
git -C third_party/riscv-tests rev-parse HEAD
