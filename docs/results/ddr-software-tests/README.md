# DDR 接口与 CPU 参数回归

2026-10-26，WSL Ubuntu-24.04，Verilator 5.020 / GCC 13.2.0。

| 记录 | 内容 |
| --- | --- |
| [bridge-and-soc.log](bridge-and-soc.log) | 三种异步时钟比例，各 656 项协议检查；CPU 执行 DDR 自检固件，8449 次外存事务、126002 周期、退出码 0 |
| [default-regression.log](default-regression.log) | 默认 MULH_PIPELINE=0 的完整 make check |
| [mulh-pipeline-regression.log](mulh-pipeline-regression.log) | MULH_PIPELINE=1 的完整 make check |
| [mulh-coremark-short.log](mulh-coremark-short.log) | 启用 MULH 寄存级的 10 次 CoreMark 短测，算法 CRC 正确、1,983,373 周期；不是正式成绩 |
| [response-select-regression.log](response-select-regression.log) | 10 月 27 日响应来源锁存、并行队列写位置选择候选；一次完整检查通过 |
| [response-select-final-ddr.log](response-select-final-ddr.log) | 新候选真实 CPU 执行 DDR 自检，8449 次事务、126002 周期、退出码 0 |
| [response-select-coremark.log](response-select-coremark.log) | 新候选 10 次短测，CRC 正确、1,983,373 周期，与原短测一致 |
| [mmio-stage-ddr.log](mmio-stage-ddr.log) | 再加入 MMIO 请求寄存级的定向检查：CPU DDR 自检通过，8449 次事务、126133 周期 |
| [mmio-stage-uart.log](mmio-stage-uart.log) | MMIO 请求寄存后 UART 连续六次写入及交错 TCM 访问通过，逐位核对实际波形 |

完整回归包括 70 项 ISA、15 类异常/CSR、机器态中断、19 组双发射、219 轮随机背压与退休差分（102240 条指令，第二通道退休数随 MULH 等待配置而异，详见各日志）和 UART 波形检查。

协议模型遵守 HMIC_S 的无 WVALID/B/RREADY 接口；仿真不包含加密 DDR PHY，也不模拟模拟电路的校准和信号完整性。可执行命令见 [test_ddr.sh](../../../scripts/test_ddr.sh)，板测结果需另行记录。


10 月 27 日后续针对性检查：`forward-data-differential.log`（退休差分通过，102240 条指令）、`forward-data-ddr.log`（DDR 自检 8449 次事务、126133 周期）、`forward-data-coremark.log`（10 次短测仍为 1,983,373 周期，CRC 正确）。这轮只覆盖本次 CPU 旁路与辅助通道 PC 修改，不重复未受影响的 UART/PHY 协议测试。


10 月 28 日仅针对仲裁授权修改运行 `grant-protocol.log`（标称时钟比例、656 项检查通过）和 `grant-ddr-soc.log`（CPU 自检 8449 次事务、132600 周期通过）。未重复未修改的 CPU 算法回归与其他时钟比例测试。
