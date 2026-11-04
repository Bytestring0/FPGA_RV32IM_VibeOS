# 全部源码（可复现）

本目录整合了本作品的全部自研代码：RTL、设备端固件、模型运行时、Agent、CoreMark 移植与构建工具。共 **199 个文件**，约 1.2 MB。

## 目录结构

```
10_全部源码/
├── rtl/                CPU / SoC / Cache / BTB / NPU 的 SystemVerilog 源码
├── fpga/board/         板级顶层、时钟、DDR 顶层与约束（.fdc）
├── firmware/
│   ├── system_sw/      设备端：启动、SD/FAT、资源加载、HDMI 文本、UI、板级应用
│   ├── model/          模型运行时：加载、量化、Q16 定点、张量 ABI、采样、分词
│   └── agent/          Agent 循环、工具协议、工具归一化
├── coremark/
│   ├── third_party/    CoreMark v1.0 官方核心（未修改）
│   └── port/           移植层：core_portme、ee_printf、start.S、link.ld
├── tools/              构建脚本（固件、资源打包、字库生成、bin2hex）
└── pds/                PDS 2025.2 工程脚本与约束（参考）
```

## 关键实现位置

| 功能 | 文件 |
|---|---|
| 五级双发射流水线、分支预测接入 | rtl/rv32_core.sv |
| 2 路组相联 I/D Cache | rtl/ag_l1cache.sv |
| BTB + 返回栈 | rtl/ag_btb.sv |
| CSR / 异常 / 中断 | rtl/rv32_csr.sv |
| DDR 异步桥与读突发合并 | rtl/ag_hmic_bridge.sv、rtl/ag_hmic_read_burst.sv |
| NPU 张量数据通路 | rtl/npu_tensor_v2.sv、rtl/npu_delta_vectors.sv |
| UART / GPIO / HDMI 文本控制器 | rtl/ 与 fpga/board/ |
| 模型加载与量化 | firmware/model/qw_load.c、qw_forward.c |
| 工具调用协议 | firmware/agent/qw_agent_protocol_backend.c |
| HDMI 菜单 / 键盘 / 跑分页 | firmware/system_sw/ag_agent_ui.c、ag_coremark_ui.c |

## 构建固件

```bash
# 需要 riscv64-unknown-elf-gcc 与 python3
python3 tools/build_agent_interactive.py \
  --output  <输出目录> \
  --config  <模型配置目录> \
  --smoke-tokens 0 --uart-assets --simple-profile
```

产物：firmware.bin（TCM 镜像）、APPRES.BIN（DDR 资源）、firmware.elf。

## 构建 CoreMark

```bash
riscv64-unknown-elf-gcc -march=rv32im_zicsr_zifencei_zba_zbb -mabi=ilp32 -O3 \
  -ffreestanding -fno-builtin -nostdlib -nostartfiles -T port/link.ld \
  port/start.S third_party/core_*.c port/core_portme.c port/ee_printf.c -lgcc -o coremark.elf
```

实测：10000 次迭代，19.45 s，514.085 CoreMark，5.040 CoreMark/MHz，CRC 全通过。

## PDS 工程

`pds/` 内的 tcl 来自已闭合工程 `npu_hot102_20261104`（102 MHz，All Constraints Met）。脚本中的文件路径是绝对路径，迁移时需改成本机路径；器件为 PG2L100H-6FBG676。

## 说明

- 模型权重文件不在本目录（388 MB W4 权重 + 510 MB CPU 侧嵌入），随位流/固件一起在 08_位流与镜像 与 SD 卡中提供。
- `tools/` 同时包含本次分析用的统计脚本（ILA、打包、清理），不影响构建。
