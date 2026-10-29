# MEM 位操作修订的功能证据（2026-10-29）

这些是 Verilator 仿真结果，没有 FPGA 下载或板测，不提供实际 Fmax。

- `make differential MULH_PIPELINE=1`：219 组、102240 条退休指令、20862 条第二路退休通过，含 RV32I/M/Zba/Zbb 和随机背压。
- `make coremark-opt MULH_PIPELINE=1 CPU_HZ=102000000 ITERATIONS=10`：1983373 个计量周期，等效 5.041916/MHz；三项 CRC 正确，少于 10 秒，不能作为正式比赛分数。
- 完整 SoC：MULH_PIPELINE=1、LOAD_WORD_ONLY=0、DDR_ENABLE=1、CACHE_ENABLE=1、CACHE_SET_BITS=8、SIM_FAST_UART=1；`sim/ddr_soc_main.cpp`，固件 `cache-firmware.hex`。DDR/Cache/自修改代码/维护测试通过；96006 周期、1232 次外部事务、热循环 4336 周期。

`board-source.sv` 是本次物理实现候选的源快照（板级参数 MULH_PIPELINE=1）；`rv32_core.sv` 与 `rv32_alu.sv` 为修改的 RTL。其他依赖保持仓库同次提交。文件 SHA-256 见 hashes.json。
