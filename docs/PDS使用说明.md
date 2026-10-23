# 在 PDS 中打开与运行 CPU 工程

适用目录：`D:\FPGA\RiscV`。本机工具为 PDS 2022.2-SP6.1，目标器件为 Logos2 `PG2L100H-6 / FBG676`。

## 为什么之前提示没有文件

原来的 `RiscV.pds` 只保存了器件选择，设计源文件列表和约束文件列表都是空的，也没有指定 CPU 顶层。之前命令行综合使用的是 `build/pds/qinglong.pds`，因此打开原来的空工程时，PDS 并不知道新写的 RTL 文件。

现在主工程引用以下文件：

| 项目 | 设置 |
| --- | --- |
| 工程文件 | `D:\FPGA\RiscV\RiscV.pds` |
| 顶层模块 | `qinglong_fpga_top` |
| 设计源文件 | `fpga/generated/qinglong_all.sv` |
| 逻辑/时序约束 | `fpga/timing.fdc` |
| 初始化程序 | 工程根目录的 `firmware.hex` |
| 综合工具 | PDS 自带 ADS，工程配置值 `selected_syn_tool_opt = 2` |

原工程首次备份在 `fpga/backups/RiscV.before_gui_sync.pds`。

## 第一次打开

1. 在 PDS 中关闭之前打开的空工程。若提示保存旧工程，不要把旧的空源文件列表重新保存到 `RiscV.pds`。
2. 用 **File → Open Project** 重新打开 `D:\FPGA\RiscV\RiscV.pds`。
3. 在设计源文件列表中确认出现 `qinglong_all.sv`，顶层确认是 `qinglong_fpga_top`。
4. 先运行 **Compile**，通过后运行 **Synthesize**。综合工具应选择 **ADS / Pango Synthesize Tool**。
5. 需要布局布线报告时，继续运行 **Device Map → Place & Route → Report Timing**。

若工程仍显示空白，请先执行下一节的同步命令，再关闭、重新打开该工程。磁盘文件变化不一定会刷新 PDS 已打开的工程状态。

## 修改 RTL 或切换固件后同步

在 PowerShell 中执行：

```powershell
Set-Location D:\FPGA\RiscV
.\scripts\sync_pds_project.ps1
```

默认使用已有的 `build/smoke.hex`。使用 CoreMark 固件：

```powershell
.\scripts\sync_pds_project.ps1 -Firmware build\coremark.hex
```

同步脚本会读取 `rtl/files.f`，补入板级顶层，将源文件按顺序合并到 `fpga/generated/qinglong_all.sv`，复制固件到根目录 `firmware.hex`，并恢复主工程的完整设置。它不会执行综合，也不会改动 `rtl/` 源码。

应修改 `rtl/*.sv`，然后重新运行同步脚本。不要直接修改 `fpga/generated/qinglong_all.sv`，下次同步会重新生成该文件。若新增一个 RTL 模块，将它按依赖顺序加入 `rtl/files.f`。

若固件还没有生成，可在 WSL Ubuntu 中构建：

```powershell
wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/d/FPGA/RiscV && make smoke'
```

CoreMark 固件的构建参数和性能测试流程以项目主说明为准。同步脚本可接收任意已经生成的 `.hex` 固件路径。

## 为什么工程只添加一个合并的 SV 文件

CPU 源码仍按译码器、预测器、乘除法、CSR、流水线、SoC 分模块维护。这里合并的是交给旧版 PDS 编译器的输入文件。

本机 PDS 2022.2 的 ADS 前端在多个独立文件之间发现 SystemVerilog package 时存在兼容问题。把 package 放在同一个编译单元的前面，可避免 `decode_t is not declared` 等报错。已经针对该版本处理了枚举声明、结构体端口和常量位宽转换的兼容性。不要同时把合并文件和原始 RTL 全部添加到同一工程，否则会出现模块重复定义。

## 与命令行验证工程的区别

GUI 主工程是 `RiscV.pds`，输出保存在主工程工作目录。命令行入口仍可使用：

```powershell
.\scripts\build_fpga.ps1
.\scripts\build_fpga.ps1 -PlaceRoute
```

它会在 `build/pds` 下重新生成独立的 `qinglong.pds`，便于保留可复现的综合与布局布线结果。两个入口采用相同 CPU RTL 和相同器件，但它们是两个不同的 PDS 工程；在一个工程中生成的报告不会自动出现在另一个工程的流程状态里。

## 时钟约束和下载前需要核对的内容

`fpga/timing.fdc` 中的高频时钟约束用于探索 CPU 时序能力。设置一个目标频率不会改变开发板实际时钟，也不会自动生成 PLL。本次板级顶层仍以参考手册中的 27 MHz 输入时钟作为启动配置，UART 的分频参数与之对应。

根工程用于编译、综合和时序分析，不含板级引脚约束。本机已通过 `scripts/prepare_board.ps1` 生成完整板级工程，并在连接的 PG2L100H 开发板完成 27 MHz smoke 验证：三次 JTAG 下载、配置校验、COM12 的 UART 输出均通过。板级引脚为 D18 时钟、C22 复位、C24 UART TX，使用 LVCMOS33。详见[上板调试记录](上板调试记录.md)。

上板时打开 `build/board27/qinglong_board.pds`，或运行：

```powershell
.\scripts\prepare_board.ps1 -ClockMHz 27 -Firmware build\smoke.hex -Build -Bitstream
.\scripts\program_board.ps1 -Port COM12 -Output build\board-uart.log
```

底板 UART USB-C 必须独立连接到电脑，COM 口以 Windows 枚举为准。FTDI 下载器的 COM10/COM11 不等同于 CP210x 的 UART。高频 PLL、其他板卡版本和断电启动需要分别验证。

## 常见报错

| 现象 | 处理 |
| --- | --- |
| 没有设计文件 / 顶层为空 | 同步主工程，关闭旧工程并重新打开 `RiscV.pds` |
| `Synplify pro license is not set` | 本机改用 ADS；同步脚本已保存该选择 |
| `decode_t is not declared` | 使用生成的单一 `qinglong_all.sv`，并开启 SystemVerilog |
| `SystemVerilog Enumeration is not supported yet` | 使用当前兼容版 RTL，不要切回未调整的旧副本 |
| `firmware.hex` 找不到 | 先构建固件，再运行同步脚本；从主工程目录运行 |
| 修改 RTL 后结果没变化 | 重新同步生成合并文件，然后在 PDS 中重新 Compile/Synthesize |
| 目标时序未满足 | 查看报告中的关键路径与 WNS，按实际报告优化或降低时钟；不能把约束目标当作已达到主频 |
