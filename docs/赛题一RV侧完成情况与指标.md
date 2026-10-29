# 赛题一 RV 侧完成情况与指标

更新：2026-10-29。依据用户提供的《紫光.pdf》物理第 10–11 页。当前为 RV 侧实现与验证交付，不能标成整套赛题全部验收通过。用户本轮未连接 FPGA；没有新增板测或 ILA 捕获。

## 逐项对照

| 赛题条目 | 当前实现 | 验证状态 / 边界 |
|---|---|---|
| 基础 1：RV32I 内核 | 自主 SystemVerilog RV32I，另实现 M/Zicsr/Zifencei/Zba/Zbb | ISA、随机差分、异常等回归；不是 RISC-V 官方认证 |
| 基础 2：至少三级流水、冲突与跳转 | 六级、双发射、旁路、背压、冒险停顿、分支冲刷、精确提交 | 双发射压力、随机背压、异常/中断检查通过 |
| 基础 3：UART/GPIO | 115200 UART TX、GPIO/LED、mtime/mtimecmp/msip | 已有实际串口结果；无 UART RX、键盘、HDMI 驱动 |
| 高阶 1：两路 I/D Cache | 独立两路 I/D，各 16 KiB 板级配置，32B 行，D 写回/写分配、LRU、维护与错误保持 | 仿真通过；已有 102 MHz 实验镜像 Cache 板测，此前镜像有时序违例；当前完整 SoC 已离线闭合，待重新板测 |
| 高阶 1：突发效率 | 256 位整行接口，接 PDS HMIC_S 与板载 32 位 DDR3；单拍 LEN=0 映射 DDR BL8 | 合并行传输已实现；不是多未决、多个 AXI beat 的突发引擎 |
| 高阶 2：动态预测 | 256 项 BTB、局部历史方向预测、16 项返回栈 | 随机差分、控制流压力与 CoreMark 验证 |
| 高阶 2：异常/中断 | 所实现 M 态 ISA 的同步异常、软件/定时/外部中断、mtvec、MRET、双提交精确恢复 | 已验证；不包含 S/U、MMU、PLIC 或完整操作系统 |
| 高阶 3：边缘 AI 与模型/显示融合 | 已审查队伍加速仓库、提出 DDR/软件接口与生成式浏览器方案 | 加速器尚未并入本 SoC，无模型推理或显示应用验收；属于后续整机工作 |
| 文档与性能分析 | CPU/Cache/DDR 文档、接口文档、原始性能报告与日志 | 已提供 |
| ILA 关键算法波形分析 | 核已导出提交、trap、握手、性能计数观察信号，接口文档给出探针和触发建议 | 未完成新工程实板捕获；须重新接板、加入调试 IP 并重新实现 |

## 指标必须按版本比较

| 版本与证据 | 主频与时序 | 资源 | 性能 |
|---|---|---|---|
| 2026-10-26 TCM CoreMark 正式板测 | CPU 102 MHz；setup +0.120 ns，slow/fast hold +0.059/+0.041 ns；All Constraints Met | 7089 LUT、2445 FF、48 DRM36K、4 DSP、1 GPLL | 6000 次 / 1,189,859,177 周期；11.665286 s；514.346581 CoreMark；5.042613543 CoreMark/MHz；0.072556 CoreMark/LUT |
| 2026-10-29 Cache 实验板测 | CPU 标称 102 MHz；setup −0.306 ns，34 个端点；DDR UI 125 MHz setup +1.357 ns；未闭合 | 14603 LUT、10480 FF、66.5 DRM36K 等效、4 DSP、3 GPLL、2 PPLL | DDR 与 Cache 自检通过；100 次热 DDR 代码调用 4336 周期；未做正式 CoreMark |
| 2026-10-29 完整 DDR/Cache SoC 离线闭合 | CPU 102 MHz，setup +0.056 ns，slow/fast hold +0.136/+0.099 ns；DDR UI 125 MHz setup +2.216 ns；All Constraints Met | 14618 LUT、10521 FF、66.5 DRM36K 等效、4 DSP、3 GPLL、2 PPLL | 当前 RTL 功能仿真通过；本版本尚无正式板测 CoreMark/LUT |
| 当前 RV RTL 仿真 | 不产生实际频率成绩 | 不能以仿真报告资源 | 短 CoreMark 10 次：1,983,373 周期，等效 5.041916/MHz，CRC 正确；不满足正式测试至少 10 秒要求 |

第一行是已经闭合且板测通过的历史 TCM 工程，不等同当前带 DDR/Cache 的源码。不能把该行的 CoreMark 除以另一版本 LUT。108 MHz 历史试验存在负裕量，不能称为闭合最高频率。频率依据 PLL 配置，不是频率计测量。

相同固件、相同 DDR 仿真模型的有/无 Cache 对照（无 Cache 数据为前一版 RTL；本次修改后有 Cache 结果复测相同）：缓存热循环 4336 周期、无缓存 21325 周期，约 4.9181 倍；全自检 96006 / 166067 周期，约 1.7298 倍；外部事务 1232 / 10570，减少 88.3444%。这些是指定工作负载的仿真结果，不是实板加速倍数或 CoreMark 成绩。

器件 PG2L100H-6IFBG676 有 66,600 LUT6，99,900 是等效 LUT4 数。完整 Cache 实验版本的 14,603 LUT 约占 21.9%；相对最初 19,175 LUT 降低约 23.8%。赛题明确评价 CoreMark/MHz、满足约束的最高频率、CoreMark/LUT；未给出可自行套用的固定权重或外围逻辑豁免规则。

## 交付与待验收工作

接口定义见 [RV侧接口文档](RV侧接口文档.md)，结构与缓存实现见 [CPU设计说明](CPU设计说明.md)、[两路Cache设计与性能](两路Cache设计与性能.md)。原始证据分别在 `results/board-102mhz`、`results/offline-102mhz`、`results/board-cache-102mhz-experimental`、`results/cache-simulation`；本次功能证据为 `results/rv-late-alu-simulation`，闭合证据为 `results/offline-ddr-cache-102mhz`。

完整 Cache SoC 已按 PDS 2022.2-SP6.1 / PG2L100H-6IFBG676 最终布线报告闭合，所有已约束路径的 setup/hold/recovery/removal/脉宽检查均无违例。采用 MEM 位操作分割、布局种子 2、多工艺角优化和关键网络优先布线；保持完整 Load 旁路、102 MHz CPU、125 MHz DDR UI、1000 MT/s DDR 与每侧 16 KiB Cache。未增加掩盖功能路径的 false path/multicycle，时钟与不确定度未放宽。

+0.056 ns 是此次实现的实际 STA 裕量；工具估计 102.5861 MHz 不是另一个已实现或已板测频率。闭合范围采用 HMIC_S 随 IP 的 PHY 约束与 CPU/UI mailbox 最大延时约束。报告仍列出 DDR 外部端口无普通 I/O delay 的 IP 接口提示；这些端口由专用 PHY、板级走线及校准负责，不能将 STA 通过说成新板测或板级信号完整性验收。

重新接板后需要：最终闭合版本 DDR/Cache 自检；同一个源代码、固件和位流对应的至少 10 秒 CoreMark；核对 LUT 分母；加入 ILA 后重新检查时序和资源、采集关键循环提交/停顿波形。AI、HTML 渲染、输入设备、自主 OS 和持久化存储尚未实现，DDR 的写入不是断电持久化。

被淘汰的候选：完整 Load 旁路、布局种子 1 的第一轮最终路由，CPU setup −0.095 ns（9 个端点），DDR PHY 另有 −0.461 ns，仍不满足整机验收。原始数据见 results/cache-timing-candidates/seed1-first。
