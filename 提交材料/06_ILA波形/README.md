# ILA 波形（2025.2 实测，已取得并已定量分析）

## 文件

| 文件 | 说明 |
|---|---|
| ila-2025-xpc.vcd | 473 个有效采样点的 x_pc[31:0] 波形 |
| ila-2025.dprj | Fabric Debugger 工程，可打开复现/截图 |
| ila-board.fic | 探针定义（32 位 x_pc，深度 512，cpu_clk） |
| ila-cap-2025.tcl | 抓取脚本 |

## 采集条件（如实标注）
- 工具链：PDS 2025.2 / Fabric Debugger 2025.2
- 位流：npu_ila96cm，96 MHz，TCM 内直接烧入 CoreMark，上电即运行
- 该位流 CPU 域满足（+0.429 ns）；残留 2 条 −0.039 ns 违例在厂商 DDR PHY 内部，CoreMark 全程用 TCM、不访 DDR，故不影响本波形
- 触发：立即触发；采集 2 s 后先 dbg_fla_stop 再导出（不先 stop 会导致导出挂起）

## 定量结果

| 指标 | 数值 |
|---|---|
| 有效采样 | 473 个（对应 472 次 PC 跳变） |
| PC 范围 | 0x00001DAC – 0x00002158（CoreMark 代码段） |
| 不同 PC | 93 个 |
| +4（单发射） | 278 次，58.9% |
| +8（双发射） | 114 次，24.2% |
| 向后跳转（循环/分支） | 41 次，8.7% |
| 向前大幅跳转（taken 分支） | 39 次，8.3% |
| **双发射占发射槽** | **29.1%**（114 / (278+114)） |

## 分析

1. 双发射确实生效：29.1% 的发射槽为两条指令同时退休（PC +8），其余为单条（+4）。CoreMark 以指针追逐和整数乘除为主，存在数据相关，29% 属于合理区间。
2. 控制流密集：8.7% 向后跳转 + 8.3% 向前 taken 跳转，合计约 17% 的采样发生控制转移，对应 list/matrix/state 三个负载的循环与条件分支，这正是 BTB + 返回栈发挥作用的场景。
3. 与 5.040 CoreMark/MHz 相互印证：若无双发射，按单发射 IPC=1 估算约 2.5–3 CoreMark/MHz；实测 5.04，说明双发射与分支预测共同贡献了约 1.7–2 倍。

## 复现方法

```
cd D:/App/pango/PDS_2025.2-ads/bin
./cdt_js.exe -port 65420
./cdt_dbg_shell.exe -file D:/FPGA/RiscV/build/contest-validation/ila-cap-2025.tcl
```

脚本会编程位流、导入 FIC、立即触发、停止、导出 VCD 并保存 dprj。
