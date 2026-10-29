# 两路 I/D Cache：102 MHz 实验板测

2026-10-29，盘古 100Pro+ / PG2L100H-6IFBG676。配置为 CPU 名义 102 MHz、DDR3 1000 MT/s、用户时钟 125 MHz，I/D Cache 各 16 KiB、两路、32 字节行。只下载 FPGA 配置 RAM，未烧写 Flash。

**功能板测通过，静态时序尚未闭合。** 该镜像 CPU slow setup WNS −0.306 ns、TNS −3.007 ns、34 个违例端点。DDR UI setup +1.357 ns，CPU slow/fast hold +0.081/+0.052 ns，其余受约束时钟、跨时钟数据、恢复/移除和脉宽检查通过。报告 CPU slow Fmax 约 98.9 MHz，不能将一次 102 MHz 功能运行当成全角时序达标。

UART 已收到完整结果，之后 JTAG 配置校验通过：

```text
DDR calibrated
DDR PASS data16KiB byte halfword address-lines edge execute
CPU MULH PASS signed mixed unsigned edges
CACHE I_HIT=00000204 D_HIT=00001efd WB=00000231 IBUF_HIT=000005ce
CACHE_WORK_CYCLES=000010f0
CACHE PASS DDR code warm-hit self-modify maintenance
```

覆盖 DDR 数据、字节/半字、地址线与边界、DDR 代码执行、12 次高位乘法边界值、100 次热代码调用、连续自修改代码及维护。热循环实测 4,336 周期，按名义 102 MHz 为约 42.51 μs；未对无缓存镜像做同固件实板 A/B 比较，仿真加速倍数不能标成实测加速。

最终布线资源：14,603 LUT、10,480 FF、66.5 DRM36K 等效、4 APM、3 GPLL、2 PPLL；LUT6 约占器件 66,600 个的 21.9%。该镜像没有运行正式 CoreMark，不能把历史 TCM 成绩除以这里的 LUT 数当成新实测 CoreMark/LUT。

来自 `build/ddr-soc102-cache16-refine/place_route/route_constraint_ite_2/`，完整下载工程为 `build/ddr-soc102-cache16-exp/`。本目录保存实际合并源码、固件、FDC、最终时序、资源、UART、下载/校验日志及位流。它基于已完成实现的冻结版本，当前继续优化的轻量预译码/MEM MINMAX 版本是另一份源码，不继承这次板测。

- Source SHA-256：`832E073759D18AB40375A4808D51183F76C5ACDF24A176DAAC6E2DED7DE4E169`
- Bitstream SHA-256：`D754EE24AC109535D0D64013138BD864D633522699787B7D83B7705140CF59BF`
- `board_tested=true` 与 `routed_timing_passed=false` 同时保留在 manifest 中。
