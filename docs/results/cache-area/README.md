# Cache 面积优化证据

2026-10-28，PDS 2022.2-SP6.1，PG2L100H-6IFBG676，CPU 102 MHz、DDR UI 125 MHz，MULH_PIPELINE=1。前后均为独立两路 I/D Cache，各 16 KiB、32 字节行。

| 指标与阶段 | before | after |
| --- | ---: | ---: |
| Cache 综合 LUT | 5844 | 1764 |
| Cache 综合 FF | 3411 | 816 |
| Cache 综合 DRM36K 等效 | 27 | 18.5 |
| 整机器件映射 LUT | 19007 | 14664 |
| 整机器件映射 FF | 13058 | 10464 |
| 整机器件映射 DRM36K 等效 | 75 | 66.5 |
| 整机最终布线 LUT | 18894 | 14541 |
| 整机最终布线 FF | 13058 | 10467 |

Cache LUT 减少 69.815%，整机映射 LUT 减少 22.849%。综合表与器件映射表按各自阶段比较，不与布局布线后的 LUT 数混算。18 Kbit 块在 DRM36K 等效统计中计作 0.5。

before 来自 `build/ddr-soc102-cache16-redirect/`；after 来自 `build/ddr-soc102-cache16-area/`。各目录保存综合/映射报告、RAM 推断明细、合并源码、固件、约束和 manifest 哈希。普通 D 读与维护扫描合并成一个读地址，消除了 D 数据/Tag 副本；元数据使用同步 RAM，复位逐组初始化，消除大位图的复位/读写选择逻辑。缓存容量、功能和命中时序不变。

after/cache-tests.log：单元检查通过，完整 CPU＋DDR 自检通过；缓存/无缓存热循环 4337/21306 周期，整体 95576/165144 周期，DDR 事务 1232/10570，与优化前一致。当前源码仿真快照见 [cache-simulation](../cache-simulation/README.md)。

板卡资源依据硬件手册 PDF 第 20 页（印刷页 16），截图与文本在 manual/。其中 LUT6=66600、等效 LUT4=99900、FF=133200、36 Kbit RAM=155。赛题 PDF 第 11 页明确考核 CoreMark/LUTs；没有在文件中给出三项性能指标的具体权重或允许扣除外设资源的规则。

完整流程重新构建于 `build/ddr-soc102-cache16-compact/`，最终为 14541 LUT、10467 FF、66.5 DRM36K 等效、4 APM、3 GPLL、2 PPLL。对照 `cache16-redirect` 的同阶段最终 LUT 减少 23.039%。CPU 102 MHz setup −0.918 ns（164 个端点）、DDR UI 125 MHz +0.345 ns，慢/快角 CPU hold +0.064/+0.050 ns；其余受约束时钟及跨域数据路径通过，仍不能作为闭合或板测成果。未生成正常下载位流。place-route.prr 与 timing.rtr 为各自的最终证据。

单独从已映射库启动 PNR 的尝试遇到 DDR CA OSERDES Place-0006，未将此结果当成时序证据。后续环形队列方案映射 14581 LUT，功能与测量周期正确，但两次布线均卡在两条连接，已停止；证据在 ring-unrouted/，该方案没有最终时序或位流，已撤回。当前 CPU 改为在原队列入队时登记配对关系与顺序预测信息，功能、性能检查通过，完整实现为 `cache16-prepair`。它的结果不能与前述 compact 结果混用。
