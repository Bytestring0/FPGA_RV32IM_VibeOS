# CPU + DDR 102 MHz 未通过的实现记录

2026-10-27。以下是诊断证据，均未作为时序闭合位流下载。

| 候选 | CPU setup WNS | DDR UI setup WNS | 结果 |
| --- | ---: | ---: | --- |
| 初始 DDR 集成，MULH_PIPELINE=0 | −0.522 ns（3 轮最好值） | 通过 | 高位乘法部分积求和到旁路/回写为关键路径 |
| MULH_PIPELINE=1，恢复布局后单轮布线 | −1.421 ns | +0.122 ns | 关键路径转为 TCM→Load 旁路→外设译码，以及双发射队列控制 |

第二版 CPU slow/fast hold 为 +0.034 / +0.018 ns；CDC 的 CPU→UI / UI→CPU setup 裕量为 +4.806 / +4.150 ns。实现后 12687 LUT、9137 FF、48 DRM、4 DSP、3 GPLL。这些不是已板测 SoC 的资源统计。

原始报告：`original-best.rtr`、`mulpipe-first.rtr`、`mulpipe-first.prr`。随后一次路由迭代在中途被中断，无新的最终报告。

响应选择、队列控制与 MMIO 请求寄存级候选 `ddr-soc102-mmio`：最终 CPU setup −0.776 ns，TNS −43.066 ns、258 个违例端点；DDR UI +1.024 ns，CPU slow hold +0.068 ns。12562 LUT、9207 FF、48 DRM、4 DSP、3 GPLL。原始报告 `mmio-stage.rtr`、`mmio-stage.prr`，未下载。

当前候选 `build/ddr-soc102-forward/` 再缩短 Load 旁路的数据选择，并提前锁存辅助通道 PC；保持 CPU 102 MHz、DDR 1000 MT/s，等待独立实现结果。


10 月 28 日 `ddr-soc102-forward` 完成布线：CPU setup −0.636 ns，DDR UI −0.029 ns；报告见 `forward-data.rtr` / `forward-data.prr`，未下载。新的主要违例为数据地址通过 DDR 仲裁影响取指 ready，下一候选 `ddr-soc102-grant` 使用寄存的轮转授权切断该组合路径。


最终三轮路由优化完成：选中第二轮，CPU setup −0.643 ns（168 个违例端点、TNS −24.182 ns），DDR UI +0.896 ns；第三轮 −0.818 ns。见 `grant-refined.rtr` / `grant-refined.prr`。该候选未下载。

冻结的早期镜像已在 102 MHz CPU / DDR 1000 MT/s 完成功能板测，CPU setup −1.022 ns，见 [实验板测归档](../board-ddr-102mhz-experimental/README.md)。功能板测和静态时序分别记录。
