RISCV_PREFIX ?= riscv64-unknown-elf-
CC := $(RISCV_PREFIX)gcc
OBJCOPY := $(RISCV_PREFIX)objcopy
OBJDUMP := $(RISCV_PREFIX)objdump
MARCH ?= rv32im_zicsr_zifencei
CFLAGS := -march=$(MARCH) -mabi=ilp32 -O3 -ffreestanding \
          -fno-builtin -ffunction-sections -fdata-sections -msmall-data-limit=0 \
          -Wall -Wextra
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS := -nostdlib -nostartfiles -Wl,--gc-sections -T sw/link.ld
VFLAGS := --cc --exe --build -j 4 -O3 -CFLAGS -O3 -Wno-fatal -Wno-UNUSEDSIGNAL \
          -Wno-UNUSEDPARAM -Wno-DECLFILENAME
PREDICT ?= 1
BTB_BITS ?= 8
DUAL_ISSUE ?= 1
LOAD_BYPASS ?= 1
EARLY_LSU ?= 1
MAX_CYCLES ?= 2000000000
# Firmware and simulator outputs are shared; keep these targets sequential.
.NOTPARALLEL:

.PHONY: smoke lint sim coremark check
check:
	$(MAKE) smoke isa traps interrupts dual-stress differential uart uart-soc
build:
	mkdir -p build
build/smoke.elf: sw/start.S sw/smoke.c sw/platform.h sw/link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) sw/start.S sw/smoke.c -lgcc -o $@
	$(OBJDUMP) -d $@ > build/smoke.dis
build/uart_soc.elf: sw/start.S tests/uart_soc.c sw/platform.h sw/link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) sw/start.S tests/uart_soc.c -lgcc -o $@
build/soc_uart/Vqinglong_soc: $(wildcard rtl/*.sv) sim/uart_soc_main.cpp Makefile FORCE | build
	verilator $(VFLAGS) --top-module qinglong_soc -f rtl/files.f \
	    -GCLOCK_HZ=27000000 '-GINIT_FILE="build/uart_soc.hex"' \
	    --Mdir build/soc_uart $(abspath sim/uart_soc_main.cpp)
.PHONY: uart-soc
uart-soc: build/uart_soc.hex build/soc_uart/Vqinglong_soc
	build/soc_uart/Vqinglong_soc
build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@
build/%.hex: build/%.bin
	python3 scripts/bin2hex.py $< $@
.PHONY: FORCE
FORCE:
build/soc/Vqinglong_soc: $(wildcard rtl/*.sv) sim/soc_main.cpp Makefile FORCE | build
	verilator $(VFLAGS) --top-module qinglong_soc -f rtl/files.f \
	    "-GSIM_FAST_UART=1'b1" "-GPREDICT_ENABLE=1'b$(PREDICT)" -GBTB_BITS=$(BTB_BITS) \
	    "-GDUAL_ISSUE=1'b$(DUAL_ISSUE)" "-GLOAD_BYPASS=1'b$(LOAD_BYPASS)" \
	    "-GEARLY_LSU=1'b$(EARLY_LSU)" \
	    --Mdir build/soc $(abspath sim/soc_main.cpp)
smoke: build/smoke.hex build/soc/Vqinglong_soc
	cp build/smoke.hex build/firmware.hex
	build/soc/Vqinglong_soc
lint:
	verilator --lint-only -Wall -Wno-fatal --top-module qinglong_soc -f rtl/files.f
sim: build/soc/Vqinglong_soc
	build/soc/Vqinglong_soc

ITERATIONS ?= 10
CPU_HZ ?= 27000000
VALIDATION ?= 0
COREMARK_OPT_FLAGS := -funroll-all-loops -finline-limit=1000 -frename-registers -mtune=sifive-7-series
.PHONY: coremark-opt
coremark-opt:
	$(MAKE) coremark MARCH=rv32im_zicsr_zifencei_zba_zbb EXTRA_CFLAGS="$(COREMARK_OPT_FLAGS)"
CORE_SRC := $(addprefix third_party/coremark/,core_list_join.c core_main.c core_matrix.c core_state.c core_util.c)
.PHONY: coremark-build deps isa
deps:
	bash scripts/fetch_deps.sh
isa: build/soc/Vqinglong_soc
	python3 scripts/run_isa.py
coremark-build: | build
	$(CC) $(CFLAGS) $(LDFLAGS) -Isw/coremark -Ithird_party/coremark \
	    -DCOMPILER_FLAGS='"$(CFLAGS)"' \
	    -DITERATIONS=$(ITERATIONS) -DCPU_HZ=$(CPU_HZ) -DTOTAL_DATA_SIZE=2000 \
	    -DVALIDATION_RUN=$(VALIDATION) -DPERFORMANCE_RUN=$(if $(filter 1,$(VALIDATION)),0,1) \
	    sw/start.S $(CORE_SRC) sw/coremark/core_portme.c sw/coremark/ee_printf.c \
	    -lgcc -o build/coremark.elf
	$(OBJCOPY) -O binary build/coremark.elf build/coremark.bin
	python3 scripts/bin2hex.py build/coremark.bin build/coremark.hex
	$(OBJDUMP) -d build/coremark.elf > build/coremark.dis
coremark: coremark-build build/soc/Vqinglong_soc
	cp build/coremark.hex build/firmware.hex
	MAX_CYCLES=$(MAX_CYCLES) python3 scripts/run_coremark.py
.PHONY: traps
build/traps.elf: tests/trap_tests.S sw/link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) tests/trap_tests.S -o $@
	$(OBJDUMP) -d $@ > build/traps.dis
traps: build/traps.hex build/soc/Vqinglong_soc
	cp build/traps.hex build/firmware.hex
	build/soc/Vqinglong_soc
.PHONY: differential
build/core/Vrv32_core: $(wildcard rtl/rv32*.sv) sim/core_main.cpp Makefile FORCE | build
	verilator $(VFLAGS) --top-module rv32_core -f rtl/files.f \
	    "-GPREDICT_ENABLE=1'b$(PREDICT)" -GBTB_BITS=$(BTB_BITS) \
	    "-GDUAL_ISSUE=1'b$(DUAL_ISSUE)" "-GLOAD_BYPASS=1'b$(LOAD_BYPASS)" \
	    "-GEARLY_LSU=1'b$(EARLY_LSU)" \
	    --Mdir build/core $(abspath sim/core_main.cpp)
differential: build/core/Vrv32_core
	EXPECT_DUAL=$(DUAL_ISSUE) python3 scripts/run_differential.py
.PHONY: interrupts
build/interrupts.elf: tests/interrupt_tests.S sw/link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) tests/interrupt_tests.S -o $@
interrupts: build/interrupts.hex build/soc/Vqinglong_soc
	cp build/interrupts.hex build/firmware.hex
	IRQ_EXTERNAL=1 build/soc/Vqinglong_soc
.PHONY: dual-stress
build/dual_stress.elf: tests/dual_issue_stress.S sw/link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) tests/dual_issue_stress.S -o $@
dual-stress: build/dual_stress.hex build/soc/Vqinglong_soc
	cp build/dual_stress.hex build/firmware.hex
	build/soc/Vqinglong_soc

.PHONY: uart
build/uart/Vuart_tx: rtl/uart_tx.sv sim/uart_main.cpp | build
	verilator $(VFLAGS) --top-module uart_tx -GCLOCK_HZ=27000000 \
	    rtl/uart_tx.sv --Mdir build/uart $(abspath sim/uart_main.cpp)
uart: build/uart/Vuart_tx
	build/uart/Vuart_tx
