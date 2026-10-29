# 102 MHz 闭合版本实板记录

2026-10-26，MES2L676-100HP，PG2L100H-6IFBG676，COM12 / 115200 8N1。

对原离线闭合位流进行一次 JTAG 配置 RAM 下载，6000 次 CoreMark performance 迭代完成，CRC 正确，随后配置校验通过。没有写外部 Flash。

| 项目 | 实测/实现结果 |
| --- | ---: |
| CPU 标称 PLL 时钟 | 102 MHz |
| 实测 mcycle 差值 | 1,189,859,177 |
| 换算耗时 | 11.665286049 秒 |
| CoreMark | 514.346581 |
| CoreMark/MHz | 5.042613543 |
| 实现后全最小 SoC LUT | 7089 |
| CoreMark/LUT | 0.072555591 |
| setup / 慢角 hold / 快角 hold | +0.120 / +0.059 / +0.041 ns |

UART 中整数秒截断导致 `Iterations/Sec=545`，不能将其作为精确成绩。以 64 位周期数和配置频率换算；没有外接频率计测量晶振。

- [UART](uart.log)、[下载与配置校验](program.log)、[计算结果](analysis.txt)、[manifest](manifest.json)。
- 对应源文件、固件、约束和时序/资源证据见 [此前离线归档](../offline-102mhz/)。本次使用其原位流，哈希未变。
- 本结果是 **TCM CPU 基准工程**，不包含新增加的 DDR 控制器或加速器面积；不得套用作完整 AI SoC 的 CoreMark/LUT。
- 当前源树新增 DDR 可选接口后，重新构建的版图需要独立验证；归档结果仅对应 manifest 的输入及位流。
