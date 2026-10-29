# 板载 DDR3 接入与验证

更新于 2026-10-28。开发板 MES2L676-100HP，器件 PG2L100H-6IFBG676。使用 **PDS 自带 Logos2 HMIC_S 1.10（DDR3 Interface）**，没有自行实现 DDR 控制器或 PHY。

## 2026-10-29 最新闭合版

完整 CPU + I/D Cache + 板载 DDR3 工程 `build/ddr-soc102-cache16-latealu-mc/qinglong_board.pds` 已获得最终 All Constraints Met：CPU 102 MHz setup +0.056 ns、hold +0.136/+0.099 ns；DDR UI 125 MHz setup +2.216 ns。两路 I/D Cache 各 16 KiB，整机 14618 LUT、10521 FF、66.5 DRM36K 等效。采用 PDS HMIC_S 1.10 与原有 DDR3 1000 MT/s 配置。

2026-10-29 接板复测：该冻结闭合版两次通过 DDR/Cache 自检，第二次完成 JTAG 回读校验，见[本次实测归档](results/board-ddr-cache-102mhz/README.md)。无完整 SoC 正式板测 CoreMark。本轮同时将项目改名为 AG，DDR IP 为 `ag_ddr3`；历史证据保留原名，当前脚本默认在 `build/ag-ddr-ip/` 生成 IP。AG 新实现的时序与实测须另行核对。以下记录保留历史开发过程。

## 历史进度与验证边界

- 原控制器确实已经生成：`build/ddr-ip/` 的 10 月 26 日生成日志为 0 error、0 warning。但旧工程布局报 `Place-0004`，生成成功不能等同于可以上板。
- 修正五处 DQS 专用脚标志后，`build/ddr-board-ip/pnr/` 独立厂商示例完成布局布线，报告 **All Constraints Met**。DDR 用户时钟 125 MHz 的内部 setup WNS 为 +2.716 ns；这是独立示例的结果，不能继承给 SoC。见[归档](results/ddr-ip-1000mts/README.md)。
- CPU 的 DDR 窗口、取指/数据适配器、跨时钟桥和板级顶层已经实现。协议仿真与真实 CPU 执行自检固件的仿真通过；加密 PHY 的校准不由此证明。
- **10 月 28 日 CPU 102 MHz + DDR3 1000 MT/s 实验镜像已通过板测**：校准、数据/字节/半字、地址线、末字、DDR 指令执行及 JTAG 校验均通过。CPU setup −1.022 ns，DDR UI +0.761 ns，整机时序仍未闭合；不得把功能通过填为时序闭合。见[板测归档](results/board-ddr-102mhz-experimental/README.md)。

## IP 参数和修正

| 项目 | 配置 |
| --- | --- |
| 板载颗粒 | 两颗 XCCC256M16EP-EINAY，兼容 MT41K256M16；每颗 4 Gbit、x16 |
| 总容量 / 位宽 | 1 GiB / 32 位，4 组 DQS |
| 控制器 | Controller + PHY，行 15 / 列 10 / bank 3 |
| 参考时钟 | 板载 125 MHz 差分时钟，N3 / N2 |
| DDR 速率 | 1000 MT/s，DDR CK 500 MHz；UI 时钟 125 MHz |
| 时序参数 | CL 7、CWL 6、AL=CL−2 |
| 物理位置 | 地址/控制 R5、数据 R4；VDDQ 1.5 V |

依据项目目录的《教程1_硬件实验指导手册》（物理页 22、25–27）、《教程2_通用实验指导手册》（物理页 139–141），以及安装目录 `UG042003_Logos2_HMIC_S_IP.pdf` 的接口章节。板载 27 MHz 晶振单独供 CPU PLL；**125 MHz 是 DDR 用户接口频率，不是 CPU 已实测频率**。

旧配置的分组和封装管脚对应基本正确，但以下实际落在 DQS 专用 PAD 的控制/地址信号仍标记为普通脚，导致 TSERDES 的分组冲突：

| 信号 | 管脚 | 改为 true 的参数 |
| --- | --- | --- |
| A14 | N1 | A14_DQS_EN |
| A5 | M1 | A5_DQS_EN |
| A11 | P1 | A11_DQS_EN |
| CKE | R1 | CKE_DQS_EN |
| CK | U6 | CK_DQS_EN |

完整配置固定在 [ag_ddr3.idf](../fpga/ip/ag_ddr3/ag_ddr3.idf)，修正前哈希和器件 PAD 对照保存在 [configuration-audit.json](../fpga/ip/ag_ddr3/configuration-audit.json)。原 `build/ddr-ip/` 保留，修正版历史输出为 `build/ddr-board-ip/`，AG 版重新生成到 `build/ag-ddr-ip/`。厂商生成的加密 RTL 留在 build，按本机 PDS 安装再生成。

## CPU 接口与故障处理

默认 `ag_soc.DDR_ENABLE=0`；DDR 工程显式设置为 1。

| CPU 地址 | 用途 |
| --- | --- |
| 0x00000000–0x0000ffff | 64 KiB TCM，复位启动、栈、陷阱及诊断代码 |
| 0x10000200 | 只读状态：bit 0 已校准且可用，bit 1 故障锁存，bit 2 已集成 DDR |
| 0x80000000–0xbfffffff | 1 GiB 无缓存 DDR，支持数据访问和代码执行 |

`ag_extmem.sv` 对取指和数据做轮转仲裁，一次只发一笔 32 字节原生事务。数据端支持字节使能；指令端返回两个 32 位指令，跨 32 字节边界时拆成两次读。最后一个 DDR 字地址只允许第一条指令有效，第二条独立返回越界错误，不回绕到 DDR 开头。

HMIC_S 是精简 AXI 接口：256 位数据拍、28 位地址；地址单位为 32 位 DDR 字，所以 `native_address = byte_offset / 4`。单拍使用 LEN=0，并对齐 8 个原生字。**没有 WVALID、B 响应和 RREADY**，不能直接接通用 AXI master。写数据必须在 AW 前准备好并保留到 WREADY；读端必须预留缓冲，不能阻塞 RVALID。

`ag_hmic_bridge.sv` 用请求/完成 toggle 握手跨越 CPU 102 MHz 和 UI 125 MHz，数据在整个握手期间保持。约束对 payload 设置 8 ns `set_max_delay -datapath_only`，同步器只切断异步输入路径；没有把 CPU/UI 的全部路径统一 false-path。完整复位必须同时复位桥和 HMIC，不能只复位一侧。

未校准时访问返回错误；校准丢失、错误 ID/LAST、事务超时进入故障锁存，后续访问返回错误，直到公共复位。CPU 域也有超时器，用于 UI 时钟停止。默认事务超时量级为 CPU 约 10.28 ms / UI 约 8.39 ms；启动固件等待校准最多 5 秒。

写响应表示 HMIC 接收了数据，不代表软件测量到 DRAM 物理落盘。DDR 是易失存储，持久化仍需要 SD/Flash。当前无缓存、无多 outstanding、无 AI/显示仲裁；这版用于可靠启动和接通存储，不代表已达到 DDR 带宽上限。

## 复现与 PDS 工程

PowerShell：

```powershell
Set-Location D:\FPGA\RiscV
.\scripts\prepare_ddr_ip.ps1
wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/d/FPGA/RiscV && make ddr-firmware'
.\scripts\prepare_ddr_board.ps1 -OutputDirectory build\ddr-soc102-grant `
  -MulhPipeline -RoutingPasses 1 -Build -Bitstream
```

当前整体候选工程路径为 **`D:\FPGA\RiscV\build\ddr-soc102-grant\ag_board.pds`**，顶层 `ag_board_top`。脚本显式注册所有 RTL、IP 和约束，解决命令行新工程“没有文件”的问题；不加 `-Build` 时仍会运行 Compile 并生成可打开的工程。已有修正版 IP 时可跳过 `prepare_ddr_ip.ps1`，不需要再次在 IP Compiler 点 Generate。默认只运行一轮布线，只有出现具体未闭合路径时才追加迭代。

若手动操作 PDS：

1. 打开上述 `ag_board.pds`，不要打开旧的空工程。
2. 检查器件 PG2L100H、-6、FBG676，综合 ADS；源列表包括合并的 `ag_board.sv` 及 HMIC 的 RTL。
3. 按 Compile → Synthesize → Device Map → Place & Route → Report Timing。整体报告通过才生成当前版位流。
4. 用 JTAG 下载配置 RAM，串口 COM12、115200、8N1。此流程不写外部 Flash。

自动下载及校验命令（仅在整体时序、位流和来源哈希检查通过后执行）：

下载脚本复用本机 65420 端口的 JTAG 服务；若未启动，按 PDS《Fabric Configuration 用户手册》3.1 节运行 `cdt_js.exe -port 65420`，无需先打开 GUI。串口仍需连接底板 CP210x；下载器枚举出的 COM10/COM11 不是 UART。

```powershell
.\scripts\program_board.ps1 -Port COM12 -Profile build\ddr-soc102-grant `
  -Output build\ddr-soc102-grant\board-uart.log `
  -Expected 'DDR PASS data16KiB byte halfword address-lines edge execute\r?\n'
```

按 KEY0（C22，低有效）触发公共复位。LED2 为可用状态，LED1 为自检正常结束，LED6 为软件退出错误，LED7 为桥故障；以 UART 结果为准。UART 先打印 CPU 配置，再打印校准状态和 PASS/FAIL。下载前打开串口，避免丢失一次性启动输出。

## 测试内容

`bash scripts/test_ddr.sh` 包含：

- 异步时钟三个比例（CPU 约 102 / 161 / 68 MHz，对 UI 125 MHz）；这里改变的是仿真时钟，不是上板频率承诺。
- 随机 AW/AR/W 背压、无等待响应、全部字节使能组合、响应停顿期间数据稳定。
- 跨数据块取指、DDR 末地址、非法地址/非对齐取指、并行指令和数据请求。
- 未校准、校准丢失、错误 ID/LAST、第二条指令独立错误、时钟停止、超时、公共复位中断事务。
- 真实 CPU 执行 `sw/ddr_smoke.c`：16 KiB 模式数据、字节/半字写、1 GiB 地址线抽查、末字访问、写入指令后 `FENCE.I` 并跳入 DDR 返回 42。

这不是全容量逐字测试、长期压力测试、温压测试或完整模型推理。后续需要扩大内存测试、重复冷启动、测量 DDR 有效带宽，再引入加速器 burst DMA 和仲裁；不能把 CPU TCM 基准的 7089 LUT 继续作为完整 SoC 面积。

## 整机时序候选：高位乘法寄存级

初始 CPU＋DDR 102 MHz 工程的关键路径在 CPU 高位乘法部分积求和、旁路/回写，三轮布线 setup 为 −0.782、−0.522、−0.670 ns，最终选择 −0.522 ns，未生成位流；DDR UI 及跨时钟 payload 均有正裕量。因此加入可选 `MULH_PIPELINE=1`：MULH/MULHSU/MULHU 在 MEM 多等待一拍，把求和与回写隔开；低位 MUL 延迟不变，默认 CPU 参数仍为 0。

启用参数及默认参数的完整功能回归均通过，见[回归归档](results/ddr-software-tests/README.md)。10 次短 CoreMark 为 1,983,373 周期，与原版本短测一致，约 5.041916 等效 CoreMark/MHz；不足 10 秒，仅用于功能和周期比较。

独立候选工程使用：

```powershell
.\scripts\prepare_ddr_board.ps1 -OutputDirectory build\ddr-soc102-mulpipe `
  -MulhPipeline -Build -Bitstream
```

最终采用哪个候选必须依据各自最终时序和板测，不把候选结果覆盖到已验证的 TCM 基准归档。

10 月 27 日恢复布局后的 MULH 流水候选最终 setup 为 −1.421 ns，未下载；DDR UI +0.122 ns，跨域 payload 与两个角的 hold 均通过。瓶颈已转到 TCM 旁路和双发射队列，见[未闭合记录](results/ddr-soc102-attempts/README.md)。因此新候选在请求接收时锁存 RAM/DDR 响应来源，并将队列的“减去弹出数再加一”写位置选择改为并行比较。功能检查及 DDR 自检仿真通过，短 CoreMark 仍为 1,983,373 周期，最终时序尚待结果。


DDR 整机还启用 `PIPELINED_MMIO=1`（默认跟随 DDR_ENABLE）：接收外设地址和写数据后，下一拍才进行寄存器译码与应答，切断 Load 旁路→地址加法→外设译码的组合长路径。TCM 访问仍走原路径；CPU 等待外设响应后才完成该访存。定向检查包括 CPU 执行 DDR 自检（8449 次事务、126133 周期）和真实 UART 六帧连续写入，均通过。`build/ddr-soc102-select/` 是被主动停止的中间候选，不作为最终时序证据。


外设流水候选实际布线 setup 为 −0.776 ns（258 个违例端点），DDR UI 为 +1.024 ns。最终候选进一步把 Load 响应是否有效的判断保留在 hazard/推进控制中，从数据选择→ALU 的串行路径中移除；未完成或出错的加载仍不能推动依赖指令退休。辅助通道 PC 在 ID 锁存，避免在旁路后的 EX 路径中插入 ±4 运算。定向退休差分、CPU DDR 自检和 10 次短 CoreMark 通过；当前工程为 `ddr-soc102-grant`，以它自己的时序报告和板测为准。


10 月 28 日旁路候选的最终 setup 为 CPU −0.636 ns、DDR UI −0.029 ns，未生成位流。关键路径已转到“数据地址→DDR 指令/数据仲裁→取指 ready→PC 更新”。当前 `ddr-soc102-grant` 将仲裁授权仅由寄存的轮转状态决定，两个请求端的 ready 不再依赖对端的当前地址/valid。空闲时遇到未请求的一方会切换授权，最多增加一拍 DDR 等待；两方持续请求时仍轮流服务。定向接口检查 656 项通过，CPU DDR 自检 8449 次事务、132600 周期通过。


## 已完成的实验上板

工程 `build/ddr-soc102-grant-refine/` 的冻结镜像已通过一次独立下载、UART 自检与随后 JTAG 配置校验。该目录这次用于保留原布线实验镜像；新的路由迭代在 `build/ddr-soc102-grant/` 进行，两个数据库不能混用。

整机资源为 12557 LUT、9232 FF、48 DRM、4 DSP、3 GPLL。CPU 与 DDR 内存通路在实板上工作，但 CPU setup −1.022 ns，仍需完成静态时序闭合；原始时序和来源哈希均随板测归档保存。`board_tested=true`、`routed_timing_passed=false` 同时记录。此固件是 DDR 自检，不提供新的 CoreMark 成绩。


## 10 月 28 日最终路由结果

三轮路由优化已结束，`build/ddr-soc102-grant/` 选中第二轮：CPU setup −0.643 ns、TNS −24.182 ns、168 个违例端点，DDR UI setup +0.896 ns。报告仍为 Some Constraints Violated，未下载该候选。证据见 `results/ddr-soc102-attempts/grant-refined.rtr` 和 `grant-refined.prr`。

当前 FPGA 保留功能板测通过的实验镜像（CPU setup −1.022 ns），与最新离线候选分开。后续应针对 TCM Load 旁路到 ALU/前端控制的路径做结构优化，再做一次必要的实现检查；无需重复已通过的 DDR 功能测试，也不应只靠继续更换路由种子来宣称闭合。

## 两路 Cache 集成

已加入独立两路 I/D Cache，DDR 板级配置各 16 KiB、32 字节行，D Cache 写回/写分配。每行匹配 HMIC 256 位单拍传输；TCM 和 MMIO 保持原访问方式。FENCE.I 和 0x10000204 维护寄存器会先写回所有脏行，再无效化指令及数据缓存。CPU＋DDR 仿真覆盖字节/半字、替换、错误处理、背压、跨行取指和连续 16 次自修改代码，均通过。详细设计、性能对照及构建方式见 [两路 Cache 设计与性能](两路Cache设计与性能.md)。

缓存适配器现使用独立 I/D 响应槽，避免前端已满时未消费的取指响应阻塞 CPU 数据事务；两个可服务请求同时存在时轮转仲裁。这里取代了上文无缓存阶段的单纯预授权方案。定向接口检查扩展为 658 项，增加“取指错误响应保持时数据请求仍完成”的检查。

首轮完整 Cache 版本 `build/ddr-soc102-cache16-release/`：19,175 LUT、13,030 FF、75 DRM36K、4 DSP、3 GPLL；CPU 102 MHz setup −0.571 ns（56 个端点）、DDR UI 125 MHz +1.188 ns，hold 通过，尚未生成正常位流。后续修订分开寄存重定向的条件和地址，并简化 TCM 读使能；219 轮差分、异常、中断和 Cache 功能检查通过，短 CoreMark 仍为 1,983,373 测量周期。新实现工程为 `build/ddr-soc102-cache16-redirect/`，结果须单独核对，不继承先前镜像的板测或时序状态。

元数据 RAM 与合并 D 读端口后，`cache16-compact` 最终布线为 14,541 LUT、10,467 FF、66.5 DRM36K 等效；CPU 102 MHz setup −0.918 ns，DDR UI +0.345 ns，两角 hold 通过。Cache 模块综合 LUT 从 5,844 降到 1,764，容量与自检性能不变。详见 [面积优化证据](results/cache-area/README.md)。CPU 当前另一个修订将取指队列改为环形结构，针对上述报告中的宽 FIFO 搬移关键路径；新实现为 `cache16-ring`，不能提前声明该版时序通过。

环形队列的原布线及一次恢复重试均卡在两条连接，已停止，未产生最终时序或位流。此阶段撤回环形结构，保留原队列拓扑，在入队时登记寄存器匹配、指令相邻关系和顺序预测标志，缩短配对判断路径。`cache16-prepair` 的差分、异常、中断、Cache 功能与短 CoreMark 均通过，周期完全不变。后续最终结果见下文。

## 2026-10-29 冻结闭合版板测与 AG 独立验证

后续 MEM 位操作拆分与多角布线候选最终在 102 MHz 闭合，CPU setup +0.056 ns / slow-fast hold +0.136/+0.099 ns，DDR UI 125 MHz setup +2.216 ns；全 SoC 14618 LUT / 10521 FF。冻结位流已两次下载通过 DDR 校准、16 KiB 数据、字节/半字、地址线/边界、DDR 代码执行、MULH/MINMAX 和 Cache 自修改/维护自检。第二次执行结束后 JTAG 回读通过，原始日志见[完整 DDR/Cache 板测归档](results/board-ddr-cache-102mhz/README.md)。这是冻结位流的功能板测，未测正式 CoreMark。

项目生产输入现已改名为 AG，合并 DDR RTL 经审计仅有命名变化，改名后 DDR/Cache 仿真和所有 CPU 回归均通过；当前 AG TCM 与独立 ILA 位流已重新闭合并上板，见[AG 赛题指标与 ILA](results/ag-contest-20261029/README.md)。改名后的完整 DDR/Cache 实现与正式成绩仍须独立核对，不能直接继承冻结位流的资源或时序。

本次改名后的完整 DDR/Cache 两个 102 MHz 重建（自检与 CoreMark 固件）在有界实现预算内未完成详细路由，未取得最终时序或可下载位流；未继续优化或重跑。原始输入与日志见[完整 AG 重建限制](results/ag-contest-20261029/README.md)。因此尚无改名后完整 DDR/Cache 的新板测或正式 CoreMark/LUT，不能继承 TCM 结果或冻结位流的闭合状态。
