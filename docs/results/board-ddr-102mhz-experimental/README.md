# 102 MHz CPU + 板载 DDR3：实验板测通过

2026-10-28，MES2L676-100HP / PG2L100H-6IFBG676，PDS HMIC_S 1.10。

**这是功能板测通过的实验镜像，CPU setup 尚未闭合。** CPU 标称 PLL 102 MHz，DDR3 1000 MT/s（CK 500 MHz、用户接口 125 MHz），两颗 x16，总地址空间 1 GiB。未用外部频率计测量。

COM12、115200 8N1 输出：

```text
Qinglong DDR3 smoke CPU=06146580
DDR calibrated
DDR PASS data16KiB byte halfword address-lines edge execute
```

下载至易失配置 RAM，UART 自检完成后 JTAG 配置校验通过；未写外部 Flash。测试包括 16 KiB 模式数据、64 字节写、32 半字写、1 GiB 地址范围的地址线抽查、0xbffffffc 末字访问、跨 32 字节边界执行 DDR 中的指令并返回 42。不是全容量逐字或长期压力测试。

CPU setup −1.022 ns，DDR UI setup +0.761 ns，hold 均通过。`routed_timing_passed=false`、`board_tested=true` 分别记录，不能把功能通过当成时序闭合。完整时序见 timing.rtr，资源见 resources.prr，来源和位流哈希见 manifest.json。镜像位于 `build/ddr-soc102-grant-refine/generate_bitstream/qinglong_board_top.sbit`。

本固件不是 CoreMark；此前 TCM 基准的 514.346581 CoreMark 和 7089 LUT 不能作为本 DDR 整机的分数和面积。
