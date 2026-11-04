# 提交材料索引（赛题一：RISC-V + FPGA 端侧 AI）

本目录按《选题指南（紫光同创）》赛题一组织。**已具备的材料直接放入；未完成的在表中标注“未完成”，不做推测性填写。**

## 一、完成度总表

| 赛题条目 | 状态 | 位置 |
|---|---|---|
| RV32I 通用处理单元 | 已实现（RV32IM + Zba/Zbb 子集） | 02_RTL源码/rv32_core.sv |
| ≥3 级流水线、数据冲突/分支处理 | 已实现（五级、双发射） | 02_RTL源码/rv32_core.sv |
| UART / GPIO | 已实现，实板用于上传与输出 | 02_RTL源码/board_ag_ddr_board_top.sv |
| 2 路组相联 I/D Cache | 已实现 | 02_RTL源码/ag_l1cache.sv |
| 总线突发传输效率 | RTL 已实现并仿真通过；**实板突发位流在重载下挂死，未验收** | 05_时序与资源/README.md |
| 动态分支预测（BTB + 返回栈） | 已实现 | 02_RTL源码/ag_btb.sv |
| 异常/中断机制 | 已实现 | 02_RTL源码/rv32_csr.sv |
| AI 加速单元 | 已实现（W4x A8，64 lane，Q16） | 02_RTL源码/npu_tensor_v2.sv |
| 完整模型/上下文/词表 | 24 层、ctx 2048、vocab 248,320 | 07_AI应用/README.md |
| CoreMark ≥10 秒 | **已实测**：10000 次，19.45 s，CRC 全通过 | 04_CoreMark/README.md |
| CoreMark/MHz | **5.040** | 04_CoreMark/README.md |
| 最高工作频率 | **102 MHz**，All Constraints Met | 05_时序与资源/ |
| CoreMark/LUT | **0.0110**（514.085 / 46,747） | 04_CoreMark/README.md |
| ILA 抓取与流水线分析 | **已完成**：2025.2 实测，473 采样，双发射占发射槽 29.1% | 06_ILA波形/README.md |
| AI 处理延迟 | 已测：TTFT 11.1 s，2.03 s/token（0.492 tok/s） | 07_AI应用/README.md |
| AI 识别率 / 任务成功率 | **未测量** | 07_AI应用/待补_识别率.md |
| 设计报告 | 已完成 | 01_设计报告/设计报告.md |
| 演示视频 | **未录制** | 09_演示视频/待补_演示视频.md |
| 全部源码 | 199 个文件（RTL/固件/模型/Agent/CoreMark/工具） | 10_全部源码/README.md |
| HDMI 截图 | **未归档** | 07_AI应用/待补_HDMI截图.md |

## 二、烧录（重要）

**PDS 不接受中文路径**。烧位流请用 `D:\FPGA\RiscV\bitstream\ag_npu_board_top.sbit`（本目录副本仅归档）。
正确顺序：先启动 UART 上传脚本监听，再烧位流触发复位，脚本即可接住 ASSET_READY。详见 08_位流与镜像/README.md。

## 三、口径说明

- 所有性能数字来自实板串口输出或 PDS 2025.2 报告，未外推。
- CoreMark 为 TCM 内独立镜像，测量期间不跑 UI；UI 内的交互跑分不是正式成绩。
- ILA 波形在 96 MHz 分析位流上采集（CPU 域满足 +0.429 ns，残留 2 条 −0.039 ns 在厂商 DDR PHY 内部，CoreMark 不使用 DDR），已在 06 中注明。
