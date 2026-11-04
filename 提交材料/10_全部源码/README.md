# 源码与产物

这里是我们自己写的代码，加上能直接上板跑的东西。构建脚本和编译中间产物没有放进来。

## 代码

`rtl/` 是 CPU 和 SoC 的 RTL。主体在 `rv32_core.sv`（五级、双发射），两路组相联的 I/D Cache 在 `ag_l1cache.sv`，分支预测在 `ag_btb.sv`（BTB 加返回栈），CSR 和异常中断在 `rv32_csr.sv`。DDR 那条路是 `ag_hmic_bridge.sv` 加 `ag_hmic_read_burst.sv`，后者把相邻的读请求合成真正的突发。NPU 的数据通路在 `npu_tensor_v2.sv` 和 `npu_delta_vectors.sv`。

`fpga/board/` 是板级顶层、时钟和 DDR 顶层，管脚和时序约束在 `board.fdc`。

`firmware/` 是设备端。`system_sw/` 管启动、SD 卡和 FAT、通过 UART 传资源、HDMI 文本和菜单界面；`model/` 管模型加载、量化、Q16 定点、张量接口、采样和分词；`agent/` 是 Agent 循环和工具调用协议。

`coremark/` 里 `third_party/` 是官方 CoreMark 源码，一行没改；`port/` 是移植层，`core_portme.c` 管计时和种子，`ee_printf.c` 是串口输出，`start.S` 和 `link.ld` 负责 TCM 启动与链接。

## 产物

`产物/` 里是直接能烧的东西：

- `ag_npu_board_top.sbit`：102 MHz 下时序全满足的位流，CoreMark 5.040 CoreMark/MHz 就是它跑出来的
- `firmware.bin`：配套的 TCM 固件，与位流一起构成板上正在运行的版本
- `APPRES_stage1/2.BIN`、`FONT_stage1/2.BIN`：两级烧录资产，第一级是位流里引导固件写死的 1 MB 字库容器，第二级才是真正的应用镜像
- `ila-2025-xpc.vcd`：2025.2 抓的 473 个 x_pc 采样，可以直接看 CoreMark 的顺序执行段和循环回跳

烧录顺序和注意事项见 `../08_位流与镜像/README.md`，别直接双击位流，PDS 不认中文路径。

模型权重太大（388 MB 的 W4 权重加 510 MB 的 CPU 侧嵌入），没有放进仓库，跟 SD 卡一起提供。
