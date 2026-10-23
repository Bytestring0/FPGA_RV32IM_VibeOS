# 青龙 QingLong：六级流水线双发射 RISC-V CPU

面向紫光同创赛题一的自主 SystemVerilog CPU 与最小 SoC。支持 RV32IM + Zicsr/Zifencei + Zba/Zbb、六级顺序流水线、动态通道交换双发射、四项指令队列、局部历史预测和返回地址栈、精确异常与机器态中断。

当前在 **108 MHz 实验配置**上两次完成 8000 次 CoreMark performance 迭代，均为 **1,586,478,654 周期、544.602348 CoreMark、5.042614 CoreMark/MHz**；实现后 7124 LUT，对应 **0.076446 CoreMark/LUT**。CRC 和配置校验通过，但 setup WNS 仍为 **−0.346 ns**，尚未时序闭合。最高同时通过时序和实板验证的归档配置为 **81 MHz、408.451697 CoreMark**。频率依据板载 27 MHz 标称晶振及 GPLL 比例，未外接频率计校准。详见[性能与验证报告](docs/性能与验证报告.md)。

## 文档

- [CPU 设计说明](docs/CPU设计说明.md)：架构、双发射、旁路、总线、CSR、地址空间及 OS/AI 接入边界。
- [青龙 OS 与即时应用设计](docs/青龙OS与即时应用设计.md)：简化 VibeOS 的方案，含 AI 计算器、AI 网页、生成式应用商店、应用脚本、DDR 与持久化；目前为设计稿。
- [性能与验证报告](docs/性能与验证报告.md)：赛题三个性能指标、周期、资源、时序、限制与复现命令。
- [PDS 使用说明](docs/PDS使用说明.md)：解决“没有文件”、同步源码与运行流程。
- [上板调试记录](docs/上板调试记录.md)：27/81/108 MHz 测试、JTAG 下载、COM12 串口及证据位置。

## 在 PDS 中运行

打开 **D:\FPGA\RiscV\RiscV.pds**，顶层为 qinglong_fpga_top，器件 PG2L100H-6 / FBG676，综合器选择 ADS。

1. 关闭之前的空工程，避免将旧的空文件列表保存覆盖新工程。
2. 重新打开 RiscV.pds，源文件列表应包含 qinglong_all.sv。
3. 运行 Compile → Synthesize → Device Map → Place & Route → Report Timing。

修改原始 rtl/*.sv 后，在 PowerShell 中同步：

```powershell
Set-Location D:\FPGA\RiscV
.\scripts\sync_pds_project.ps1
```

该命令合并十个 RTL 文件到 fpga/generated/qinglong_all.sv，复制 smoke 固件到根目录 firmware.hex，并写入完整的 PDS 工程设置。只添加合并文件可规避旧版 PDS 的 package 兼容问题；不要同时添加原始模块。

独立命令行综合与布局布线：

```powershell
.\scripts\build_fpga.ps1
.\scripts\build_fpga.ps1 -PlaceRoute
```

根工程顶层使用原始 27 MHz 时钟与对应 UART 分频。根工程约束为 110 MHz 时序探索目标，不包含板级引脚分配，也不会自动生成 PLL；实板请使用 `scripts/prepare_board.ps1` 生成的完整板级工程。当前 FPGA 最后下载的是 `build/board108-shiftdiv/` 实验镜像，已闭合的 81 MHz 镜像保留在 `build/board81-coremark/`。源码、固件和时序按各自 manifest 核对，不能混用。外部 Flash 固化及断电启动尚未执行。

## 编译与仿真

依赖 WSL Ubuntu、make、g++、python3、Verilator、gcc-riscv64-unknown-elf 和 binutils-riscv64-unknown-elf。工具链虽叫 riscv64，编译明确选择 RV32/ILP32。

```bash
cd /mnt/d/FPGA/RiscV
make deps                        # 固定版本 CoreMark / riscv-tests
make check                       # ISA、异常、中断、双发射、差分、UART 波形
make coremark                    # RV32IM 普通 -O3 基线
make coremark-opt                # Zba/Zbb 与优化编译配置，短测 10 轮
make coremark-opt ITERATIONS=6000 CPU_HZ=110000000
make coremark-opt ITERATIONS=6000 CPU_HZ=110000000 VALIDATION=1
make smoke                       # 恢复仿真启动镜像
```

coremark-opt 使用完整上游算法，附加 -funroll-all-loops、-finline-limit=1000、-frename-registers、-mtune=sifive-7-series。最后一个选项仅调整 GCC 调度模型，不引入第三方 CPU IP。日志保存实际编译参数和三个算法 CRC。

CPU_HZ 仅用于计时换算；上板时必须等于实际主频。6000 次迭代用于按 110 MHz 换算时超过 10 秒的逻辑时间验证，仍然属于仿真结果。短测会显示上游持续时间不足提示，测试脚本只允许该提示，不忽略其他错误。

| Make 参数 | 默认 | 含义 |
| --- | ---: | --- |
| DUAL_ISSUE | 1 | 顺序双发射，0 为单发射 |
| LOAD_BYPASS | 1 | 成功的 MEM Load 响应旁路 EX |
| EARLY_LSU | 1 | EX 发起访存，MEM 接收同步响应 |
| PREDICT | 1 | 分支预测 |
| BTB_BITS | 8 | 256 项 BTB；可比较 6/8/9 |
| ITERATIONS | 10 | CoreMark 迭代次数 |
| VALIDATION | 0 | 0 为 performance，1 为 validation 种子 |
| CPU_HZ | 27000000 | 固件计时换算参数 |

Make 参数控制仿真；PDS 使用 RTL 默认参数。各 SoC 测试共享 build/firmware.hex，请顺序运行，不要在多个终端同时启动 make coremark/smoke/isa 等目标。

## 工程结构与交付边界

| 目录 | 内容 |
| --- | --- |
| rtl/ | CPU、ALU、译码、预测器、CSR、除法、SoC、FPGA 顶层 |
| sim/ | SoC 模型、独立退休参考模型、UART 波形检查 |
| tests/ | 异常、中断、双发射和 MMIO 专项 |
| sw/ | 启动、链接脚本、示例、CoreMark 移植层 |
| scripts/ | 固件生成、差分、PDS 构建和同步 |
| fpga/ | 工程模板、合并源码、约束 |
| docs/results/ | 选定版本的验证日志、资源及时序证据 |
| build/ | 可重新生成的中间文件与实验日志 |
| third_party/ | 固定版本基准/测试代码及许可证 |

最小 SoC 集成 64 KiB TCM、UART TX、GPIO、定时器和软件中断。操作系统、AI 加速器、Cache/DDR、键盘/HDMI、UART RX 与持久化存储尚未实现；这些是后续自然语言交互 SoC 的工作。
