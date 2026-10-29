# 102 MHz 基准工程的离线实现快照

保存 2026-10-25 归档时的源文件、固件、约束、构建命令、回归日志及 PDS 实现报告。

- `qinglong_board.sv`、`firmware.hex`、`board.fdc` 与 `manifest.json` 对应。
- `timing.rtr`：102 MHz，All Constraints Met；setup +0.120 ns，慢/快角 hold +0.059/+0.041 ns。
- `implemented-resources.prr`：7089 LUT、2445 FF、48 DRM36K、4 DSP、1 GPLL。
- `coremark-simulation.log`：6000 次仿真、1,189,859,177 周期，CRC 正确。

这里的 manifest 保留离线归档时 `board_tested=false` 的原始状态。**同一位流已在 2026-10-26 上板通过**，新增记录见 [board-102mhz](../board-102mhz/README.md)。归档源文件独立于当前开发树，不能用新源码的实现结果覆盖这份证据。
