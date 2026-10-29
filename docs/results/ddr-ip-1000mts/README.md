# DDR3 独立示例离线验证

2026-10-26；PDS 2022.2-SP6.1、Logos2 HMIC_S 1.10，PG2L100H-6IFBG676。使用仓库冻结的 IDF（五处 DQS 标记修正）生成 IP，结合板卡 DDR 引脚进行独立厂商 BIST 示例布局布线。

- [generate.log](generate.log)：生成 0 error、0 warning。
- [timing.rtr](timing.rtr)：All Constraints Met；DDR UI 125 MHz 内部 setup WNS +2.716 ns。
- [route.prr](route.prr)：完整布局布线资源与过程。
- [example.fdc](example.fdc)、[verify.tcl](verify.tcl)：此轮约束和命令。Tcl 路径相对于 `build/ddr-board-ip/pnr/`。

报告仍有厂商示例的未约束 I/O 提示；PHY 采样依赖专用硬件及运行时校准。上述结果证明配置修正后能完成离线实现，**没有证明该独立示例通过实板 DDR 校准或读写，也不等于 CPU＋DDR 整体工程闭合**。示例中的额外 LED/复位引脚不是整体 SoC 上板引脚方案；实际下载使用独立 SoC 顶层及其板级约束。
