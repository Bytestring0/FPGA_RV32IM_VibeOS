# CoreMark v1.0 实测结果

## 测量条件
- 执行位置：**64 KiB TCM**（独立镜像，测量期间不运行 UI/外设轮询）
- 迭代次数：10000（满足赛题“运行时间不低于 10 秒”）
- 编译：riscv64-unknown-elf-gcc 13.2.0，`-O3 -march=rv32im_zicsr_zifencei_zba_zbb`
- 时钟：102 MHz（PDS 2025.2 时序已闭合）

## 结果（实板串口原始输出）
```
CoreMark Size    : 666
Total ticks      : 1984104691
Total time (secs): 19
Iterations       : 10000
seedcrc          : 0xe9f5
[0]crclist       : 0xe714
[0]crcmatrix     : 0x1fd7
[0]crcstate      : 0x8e3a
Correct operation validated.
MEASURED_CYCLES_HEX=00000000764308f3
```

## 汇总
| 指标 | 数值 |
|---|---|
| 周期数 | 1,984,104,691 |
| 时间 | 19.45 s（≥10 s 要求满足） |
| CoreMark | **514.085** |
| CoreMark/MHz | **5.040** |
| LUT（综合） | 46,747 |
| CoreMark/LUT | **0.0110** |

## 待补
- 本目录需要放入**本次 2025.2 位流对应的完整串口原始日志文件**（当前结论来自抓取输出，日志尚未归档到本目录）。
- UI 菜单内的交互跑分（DDR + 按键轮询）与上表不是同一配置，不能作为正式成绩。
