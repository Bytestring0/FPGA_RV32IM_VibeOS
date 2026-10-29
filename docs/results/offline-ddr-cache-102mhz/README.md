# 完整 DDR/Cache SoC：102 MHz 离线闭合

日期：2026-10-29。器件 PG2L100H-6IFBG676，PDS 2022.2-SP6.1 build 138931。

最终 `report_timing` 为 **All Constraints Met**。全部已约束路径的 setup、hold、recovery、removal、最小脉宽检查均无违例。当前没有连接 FPGA，`board_tested=false`；本位流没有正式板测 CoreMark 成绩。

| 项目 | 结果 |
|---|---|
| CPU 时钟 | 102 MHz，27 MHz × 34/9；不是 ×4 |
| CPU setup WNS / TNS / 违例端点 | +0.056 ns / 0 / 0 |
| CPU hold 慢 / 快角 | +0.136 / +0.099 ns |
| DDR UI | 125 MHz，setup +2.216 ns |
| DDR3 | 板载 1 GiB、32 位、1000 MT/s；PDS HMIC_S 1.10 |
| Cache | I/D 各 16 KiB、两路、32B 行，D 写回/写分配 |
| 整机 LUT / FF | 14618 / 10521（PDS 顶层模块汇总列） |
| DRM36K 等效 / DSP / GPLL / PPLL | 66.5 / 4 / 3 / 2 |
| CPU 参数 | 双发射，完整 Load 旁路，MULH_PIPELINE=1 |
| 位流固件 | DDR/Cache 自检；不是 CoreMark 固件 |

通过版本采用 MEM 位操作分割、布局种子 2、显式多工艺角优化、100 ps hold 修复目标、禁用 control chain。3 轮路由中第 2 轮通过，最终保留数据库 SHA-256 与第 2 轮完全相同，随后重新生成最终时序报告，再通过源文件/固件/约束/报告检查生成位流。第 1/3 轮 CPU setup 分别 −0.131/−0.114 ns，没有用于交付位流。

没有放宽时钟、不确定度或新增掩盖功能路径的时序例外。工具估计 102.5861 MHz 不是另一个已实现的频点。DDR 外部端口仍有无普通 I/O delay 的 IP 接口提示；STA 采用 HMIC_S 厂商 PHY 模型与约束，板级走线/信号完整性、实际校准与长期稳定性需要接板确认。

## 文件和关联证据

- `timing.rtr`：最终完整多角报告；`resources.prr`：最终资源报告。
- `qinglong_board.sv`、`firmware.hex`、`board.fdc`：冻结输入。
- `qinglong_board_top.sbit`：从通过的数据库生成的待上板自检位流。
- `manifest.json`：运行参数、输入/厂商源文件/数据库/位流 SHA-256，`board_tested=false`。
- `hashes.json`：本归档文件校验值。`build.tcl`、`priority.rcf`、`finish-bitstream.tcl`：原始实现记录；路径对应本机，应通过下面脚本在新工作区重新生成。
- [当前 RTL 功能证据](../rv-late-alu-simulation/README.md)：219 组退休差分、短 CoreMark、完整 Cache SoC 仿真。短 CoreMark 等效 5.041916/MHz；不能把历史 TCM 的正式分数除以本版 LUT。

## 重新实现

在仓库根目录的 PowerShell 中执行，PDS 安装位置不同则传入 `-PdsRoot`：

```powershell
wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/d/FPGA/RiscV && make cache-firmware'
.\scripts\prepare_ddr_ip.ps1
.\scripts\prepare_ddr_board.ps1 -OutputDirectory build\ddr-soc102-cache16-latealu-mc -Firmware build\cache_smoke.hex -MulhPipeline -PlacementSeed 2 -OptimizeMultiCorner -DisableControlChain -HoldMarginPs 100 -RoutingPasses 3 -RoutingTimeoutMinutes 60 -Build -Bitstream
```

GUI 工程为 `build/ddr-soc102-cache16-latealu-mc/qinglong_board.pds`。根目录 `RiscV.pds` 为通用 TCM 工程，不能拿其报告代表完整 DDR/Cache 版。工具或输入有变化时，必须以重新生成的报告为准。

## 重新接板后

先接电源、JTAG、USB 串口，确认串口号。下面命令仅供接板后执行，本次未运行；将 COMx 替换成实际串口。脚本在 JTAG 下载前检查来源哈希和时序报告，只写 FPGA 配置，不写启动 Flash。

```powershell
.\scripts\program_board.ps1 -Profile build\ddr-soc102-cache16-latealu-mc -Port COMx -Expected 'CACHE PASS DDR code warm-hit self-modify maintenance\r?\n' -Output build\closed-cache-uart.log -TimeoutSeconds 120
```

预期依次看到 DDR calibrated、DDR PASS、CPU MULH PASS、CPU MINMAX PASS、CACHE PASS。随后另行构建至少 10 秒 CoreMark 固件，在同一版硬件上测量；加入 ILA 后重新实现并捕获关键循环波形。修改固件或加入 ILA 后产生的位流必须重新记录哈希，不能冒用本自检位流的验收记录。
