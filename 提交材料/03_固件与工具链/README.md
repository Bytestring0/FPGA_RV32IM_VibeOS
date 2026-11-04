# 固件与工具链

## 目录
- `sw/`：设备端驱动、UI、Agent 适配、资源加载
- `agent/`：工具协议、Agent 循环、工具归一化
- CoreMark 源码在 `../04_CoreMark/src`

## 工具链
- 编译器：`riscv64-unknown-elf-gcc` 13.2.0（`-march=rv32im_zicsr_zifencei`，CoreMark 用 `+zba+zbb`）
- FPGA：PDS 2025.2
- 构建：`AI加速器/npu/system/tools/build_agent_interactive.py`

## 待补
- 一键复现脚本尚未整理进本目录（当前仍在仓库 tools 下运行）。
