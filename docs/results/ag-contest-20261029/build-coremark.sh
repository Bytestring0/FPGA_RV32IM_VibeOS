#!/usr/bin/env bash
set -euo pipefail
cd /mnt/d/FPGA/RiscV
make coremark-build ITERATIONS=6000 CPU_HZ=102000000 MARCH=rv32im_zicsr_zifencei_zba_zbb EXTRA_CFLAGS='-funroll-all-loops -finline-limit=1000 -frename-registers -mtune=sifive-7-series' > build/contest-validation/coremark-build.log 2>&1
cp build/coremark.hex build/contest-validation/coremark-102mhz-6000.hex
cp build/coremark.elf build/contest-validation/coremark-102mhz-6000.elf
cp build/coremark.dis build/contest-validation/coremark-102mhz-6000.dis
echo 'CoreMark 6000 iterations firmware built for 102 MHz'
