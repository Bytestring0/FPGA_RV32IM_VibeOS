# CPU＋NPU 优化与集成交接

资料基线日期：2026-11-02；交接整理：2026-11-03。接手人：CPU 负责人。后续目标：完成 NPU 性能优化、I/D Cache 整机闭合、模型质量验证和 VibeOS 类交互应用集成。

## 后续优化

2026-11-03最新低资源架构和融合结果见[架构重构检查点](docs/npu/AREA_REARCH_20261103.md)，剩余算子预算见[融合后预算](docs/npu/FUSED_REMAINING_BUDGET_20261103.md)。较早修改见[优化检查点](docs/npu/OPTIMIZATION_20261103.md)。本交接正文保留为导入基线，后续状态以最新检查点和实际配对报告为准。

## 先看这里

- 本分支基于远程 `main` 的 CPU 提交 `f7f87ead59bc527861084e847b5975e9929bb152`。CPU 已执行 `pull --ff-only`；同日交接时没有新增上游 CPU 提交。
- 当前 NPU、整图模型适配和真实 Agent 后端已具备源码；**最新整机没有时序闭合、没有新位流板测 token/s、模型质量也未批准**。它是可以继续开发的工程，不是已经达到交互要求的发布版。
- 用户确定的顺序：**性能与 I/D Cache 功能/一致性优先，最终物理时序随后集中解决；质量随性能修改同步检查；SD 冷启动单列。**
- 只交付当前源码、必要的集成依赖、集中验证入口和少量相关证据。模型大文件、旧交付包、旧工程、构建缓存、旧无关用例不在新增提交中。远程 CPU 原有文件保留。
- [当前优化评估](docs/npu/ASSESSMENT_20261102.md)已核对其他模型的建议。**不要把 Delta 的 q/k 常数 512/5793 改成 5793/65536**；原建议混淆了 L2 归一化与 RMSNorm。

## 工程目录

远程仓库本来以 CPU 为根目录，因此没有再嵌套一份 `CPU/`：

```text
rtl/、sw/、sim/、fpga/、scripts/   最新 CPU 及必要集成修改
AI加速器/
  rtl/compute/                   矩阵设备依赖的基础计算模块
  npu/rtl/                      保留模块依赖/可选 legacy 诊断接口
  npu/system/
    rtl/                        矩阵、tensor、SD、仲裁、整机顶层
    sw/                         当前驱动、数值参考、板端入口
    integration/
      agent_runtime_20261101/    整图模型与真实 Agent 后端
      agent_protocol_20261102/   构建实际使用的完整 R4 文本
    tools/                      当前构建与集中验收
    pds/                        真实 PDS 工程生成/检查
    config/                     模型资源清单与张量目录
docs/npu/                       当前评估、源码清单、验证摘要与相关证据
NPU_HANDOFF.md                  本交接入口
```

NPU 工具已适配该目录布局。`tools/system_common.py` 和 `tools/deployment.py` 只保留当前入口需要的公共函数；不依赖旧 `WORK/` 脚本、旧交付目录或原机器绝对路径。

本次入库文件及其 Git blob SHA256 见 [SOURCE_MANIFEST.json](docs/npu/SOURCE_MANIFEST.json)。该清单只列本次交接的新增/修改文件，不重复列远程原有 CPU 文件。

## 已完成什么

| 部分 | 当前实现 |
| --- | --- |
| 矩阵 NPU | signed INT4×INT8、G128，16/32/64 lane 可配置；激活片上预加载，权重 DDR 读取，带行/组标签的 INT32 partial 写回；保持 raw-v1 合同 |
| tensor-v2 | 20 类整数操作，覆盖向量运算、量化、Norm、非线性、RoPE、conv、Delta、attention、动态矩阵及 raw-W4 融合尺度归约；Delta 新增 Q24 输出，保留旧 Q16 合同 |
| DDR DMA | 矩阵和 tensor 各自直接请求 DDR，通过两级仲裁与 CPU Cache 共享总线；**CPU 不逐字搬运模型权重给 NPU** |
| DDR 并发 | 深度1和4可配置；四事务桥、请求/响应 CDC 队列、响应预留与 owner FIFO；四事务不等于权重预取已完成 |
| I/D Cache | 独立两路组相联，各16KiB，32B行；板级启用；TCM/MMIO旁路；全维护和按范围 clean＋invalidate |
| 驱动 | raw 接口、tensor submit/poll/wait/complete，显式授权区、缓冲租用、完成 acquire、错误及 UNKNOWN 恢复 |
| 整图适配 | Q16 向量、Q24 状态/Delta 输出等明确格式，完整 forward 与可恢复模型 FSM；head 每输出 token 的量化从61次降至1次 |
| Agent | 真实模型生成、严格工具 normalizer、实际工具回执、cooperative backend；UI 仅有可选外部 hook，未实现完整显示/桌面 |
| 会话状态 | 完整 checkpoint 和精确前缀恢复接口；当前2048-context Agent 内存布局没有启用前缀快照 |
| SD/装载 | SPI/PIO，硬件CRC16及软件回退、CMD18/CMD12与CMD17回退、FAT多块读取、chunk流式SHA、冷暖统计；没有SD→DDR DMA或原生4bit |

主要入口：

- [整机顶层](AI加速器/npu/system/rtl/ag_npu_system.sv)、[矩阵设备](AI加速器/npu/system/rtl/npu_dma_top.sv)、[tensor设备](AI加速器/npu/system/rtl/npu_tensor_v2.sv)。
- [raw 接口](AI加速器/npu/system/sw/npu_raw.h)、[tensor 数值合同](AI加速器/npu/system/sw/npu_tensor_v2.h)、[异步驱动](AI加速器/npu/system/sw/npu_tensor_driver.h)。
- [模型适配](AI加速器/npu/system/integration/agent_runtime_20261101/sw/model/qw_tensor_v2.c)、[模型 FSM 接口](AI加速器/npu/system/integration/agent_runtime_20261101/sw/model/qw_tensor_async.h)。
- [Agent 后端](AI加速器/npu/system/integration/agent_runtime_20261101/sw/agent/qw_agent_protocol_backend.c)、[整机板端入口](AI加速器/npu/system/sw/tensor_agent_cooperative_board.c)。

## CPU 集成改动

| 文件 | 改动目的 |
| --- | --- |
| `rtl/ag_soc.sv` | 默认关闭的新外设接口、外设响应队列、范围 Cache 维护 MMIO |
| `rtl/ag_l1cache.sv` | 范围维护、写回 ACK 后失效、故障时保留 dirty 数据 |
| `rtl/ag_hmic_bridge.sv` | 默认单事务兼容，显式深度4队列与 CDC |
| `fpga/board/ddr_cdc.fdc` | 单事务 CDC 约束修正 |
| `fpga/board/ddr_queue4_cdc.fdc` 及 audit/check 脚本 | 四事务跨域约束和真实综合网表审计 |
| `sim/cache_range_main.cpp` | 当前范围维护边界/故障集中验证 |

生产 `rtl/rv32_core.sv` 未因本次 NPU 交接修改。CPU-only 默认不自动开启 NPU；完整配置由 NPU 整机顶层和 PDS 入口选择。

### Cache与DMA一致性

- CPU DDR 窗口为 `0x80000000..0xbfffffff`，设备接收 DDR byte offset `0..0x3fffffff`；两者不要混用。
- 当前不存在 `0xc0000000` 非缓存 DDR 别名。`volatile` 不能替代缓存维护。
- 提交前 publish 覆盖设备要读/写的缓冲；完成后 acquire 输出和状态，之后才发布成功。NPU 运行期间 CPU 不得写租用区域。
- 范围维护按32B向外对齐，半开区间 `[start,end)`，最多6个span，先校验再合并。硬件仍扫描所有set，按tag筛选命中行；不是 O(范围内行数) 的直接索引维护。
- `0x10000200`：状态，bit5 Cache存在、bit6范围维护能力；`0x10000204`：全维护；`0x10000228`：范围版本标识；`0x10000230/234/238`：范围起点/终点/控制。
- dirty 写回失败不清 dirty；timeout/UNKNOWN 不代表 DMA 已停止，不能释放或复用缓冲。必须先证明设备与 Cache 静止，再按驱动合同恢复。
- matrix MMIO=`0x10004000`，SD=`0x10005000`，tensor=`0x10006000`；TCM/MMIO均不经过普通DDR Cache。

## 已有证据与限制

以下为原工程冻结记录；本次目录适配后的新验证结果另见 [HANDOFF_VALIDATION.json](docs/npu/HANDOFF_VALIDATION.json)。历史报告内的绝对路径和 build 路径仅用于溯源，不表示该目录已上传。原始文件 SHA/来源见 [PROVENANCE.json](docs/npu/evidence/PROVENANCE.json)。

| 项目 | 已有结果及边界 |
| --- | --- |
| tensor RTL | 历史最终报告记录531场景；本次当前源码集中重验的场景数以新报告为准 |
| CPU/Cache/DDR | 单/双发射、64 lane、四事务配置的集中仿真已记录通过，观察到 peak_pending=2；不是整模型上板 |
| Cache | 范围维护和跳过维护的负对照有报告；CPU 独立 Cache 的历史板测不能替代最新 NPU 整机验收 |
| head复用 | 8 token 少480个tensor任务，raw任务不变，8个输出hash一致；除首token外不是完整logits文件逐字节比较 |
| 整图误差 | host 8 token，首token logits relative L2≈19.73%，argmax基线220→候选13，质量未批准；host≈0.959token/s不是FPGA吞吐 |
| 完整Agent固件 | 历史静态TCM120032B，预留8KiB栈后余2848B；真实完整调用链的栈高水位未知 |
| 最新综合 | 81.6883MHz估频、setup+0.104ns，LUT32961/DRM142/APM102；该RTL尚未完成最终PnR |
| 历史同源旧PnR | 已布通，但CPU时钟域WNS−2.299ns、DDR PHY hold最低约−0.744ns；没有据此生成或下载新位流 |

仿真工具默认采用 `portable_metadata` 临时 CPU 副本以兼容当前 Verilator；报告记录原核/临时副本身份，生产核不改。这不是形式等价证明。可用支持的工具另跑 `--cpu-mode original`。

当前 host 对照是 W4 body＋BF16 head 基线与整数＋W4 head 候选，不是官方全 BF16 模型。此前完整 R4 任务曾因重复 XML 闭标签失败，无工具执行；不要放宽 normalizer、填预设答案或直接把采样策略变化当作质量修复。

### 历史计时口径

旧81MHz板端记录：raw区间410170663 CPU周期，设备busy408577718周期。冻结工程矩阵引擎也在81MHz域，因此约为5.064秒和5.044秒，差额约0.020秒；不能按125MHz换算并宣称1.79秒CPU调度开销。新整图每token约4671个tensor任务＋265个raw任务，仍需重新测其调度成本。

## 如何继续构建和验证

### Linux/WSL：集中验收

需要 Python3、GCC/G++、make、Verilator、xPack RISC-V GCC 15.2.0-1（Linux版，前缀 `riscv-none-elf-`）。CPU原有检查按根目录Makefile运行。本入口不需要模型大文件，不烧录、不读写实卡、不跑长Agent推理：

```bash
NPU_TC=/path/to/xpack-riscv-none-elf-gcc-15.2.0-1
python3 AI加速器/npu/system/tools/validate_handoff.py \
  --toolchain "$NPU_TC" \
  --output "$PWD/AI加速器/npu/system/build/handoff_acceptance_local"
```

输出目录必须是新的。该批次验证当前整数RTL及独立Fraction参考、真实RV32/Cache/DDR、Cache边界与负对照、四事务桥、异步驱动、流式装载及完整Agent固件构建。保留这些当前用例是为了能直接继续优化后复验，不是重新上传旧用例档案。

### Linux/WSL：完整Agent固件

```bash
python3 AI加速器/npu/system/tools/build_tensor_agent_cooperative.py \
  --toolchain "$NPU_TC" \
  --output "$PWD/AI加速器/npu/system/build/agent_candidate_local" \
  --ddr-resources --sd-multiblock
```

得到 `firmware.hex`、ELF/MAP、必须配套的 `APPRES.BIN` 及报告。构建只使用 tokenizer 固定元数据，不要求读取13MiB tokenizer；**并未因此验证模型资源存在或正确**。板端真实装载保留SHA验证。`APPRES.BIN`由完整R4文本和原LUT生成，必须与该次固件配套；不要复用其他构建的APPRES。

### Windows：PDS工程

生成DDR IP使用CPU已有入口，需安装匹配的PDS、Logos2 HMIC_S 1.10和有效许可证：

```powershell
.\scripts\prepare_ddr_ip.ps1 -PdsRoot 'D:\pango\PDS_2022.2-SP6.4' -OutputDirectory 'build\ag-ddr-ip'
python AI加速器/npu/system/pds/build_system.py --run npu_candidate_local `
  --firmware AI加速器/npu/system/build/agent_candidate_local/firmware.hex `
  --ip build/ag-ddr-ip --pds 'D:\pango\PDS_2022.2-SP6.4' `
  --clock 81 --tcm-kib 128 --tensor-v2 --npu-lanes 64 --ddr-outstanding 4 `
  --simulation-report AI加速器/npu/system/build/handoff_acceptance_local/cpu/report.json
```

默认只准备工程；增加 `--build --mode synthesis` 或 `--build --mode pnr` 才运行对应流程。工程写入已忽略的 `npu_system_pds/`。必须使用同源、同配置的仿真报告，不能借历史PASS。默认PDS物理动作有15分钟限时，可显式调整；PHY hold修复仍是待办。

目前板端固件按81MHz换算timer/SD分频。若修改主频，必须同步固件与时钟配置，不能只改约束或用综合估频换算性能。I/D Cache继续保持启用。

## 模型和SD资源如何交接

[config/model_assets.json](AI加速器/npu/system/config/model_assets.json)列出文件大小、SHA256、SD短文件名及原本机来源。Git中只包含小型张量目录，不包含近900MB权重/CPU资源或tokenizer；接手人需要现有SD卡或这些同SHA文件。

| host目录文件名 | SD卡8.3文件名 | 用途 |
| --- | --- | --- |
| `HEADW4.BIN` | `HEADW4.BIN` | W4 body/head参数包 |
| `CPU_RAW.BIN` | `CPU_RAW.BIN` | BF16 embedding等CPU资源，不是官方BF16 body |
| `TENSOR_HEAD.JSON` | `HDTENSOR.JSN` | 精确张量目录；仓库config中也有同SHA副本 |
| `TOKEN.BIN` | `TOKEN.BIN` | 当前248320词表tokenizer |
| 当前构建的`APPRES.BIN` | `APPRES.BIN` | 固件配套只读R4/LUT资源 |

暖推理要求模型常驻DDR。当前SD为SPI/PIO；已有多块和CRC优化不能称为SD DMA。不要格式化或覆盖非匹配文件来“自动修复”部署。

使用独立资源目录运行8-token host对照：

```bash
python3 AI加速器/npu/system/tools/run_tensor_model_v2.py \
  --assets /path/to/verified-model-assets \
  --output "$PWD/AI加速器/npu/system/build/model_check_local" --decode-count 8
```

只编译host后端可使用 `--build-only`，无需 `--assets`。完整工具任务入口为 `run_tensor_agent_v2.py --assets ... --output ...`，但当前已有长任务失败证据；应先定位数值和失败前缀，避免直接反复跑几十分钟的相同失败。

## 接下来做什么

1. **I/D Cache整机闭合**：保留启用，复核真实Agent驱动路径、CPU publish→NPU→CPU acquire、并发与UNKNOWN恢复；测命中、未命中、写回和维护成本。范围维护已实现，重点是最新整机验收与减少重复扫描。
2. **矩阵供数与并行度**：保留16-lane基线，明确启用64-lane性能候选；实现权重预取/缓冲和读算重叠，再评估融合raw归约以减少partial往返。四事务队列本身不会自动提高单任务供数。
3. **跨算子复用**：复用body公共输入量化，减少staging复制和小任务提交；明确数据所有权后减少维护次数，不能直接删flush或伪造完成。
4. **同步质量检查**：匹配head策略进行逐层/逐阶段对照，补独立高精度参考和留出任务；必要时研究更合适的动态范围或敏感矩阵格式。未实现W8/A16，不能当成现有模式。
5. **最终物理与交互验收**：解决CPU域setup及DDR PHY hold，完成同源位流/下载/UART与持续token/s；补完成IRQ、调度最坏让出时间及UI。cooperative入口不是100ms响应上界证明。
6. **SD冷启动单独优化**：SD→DDR DMA、校验/读取重叠、原生4bit仍需实现；分别报告冷启动与暖decode。

内存约束：模型arena从`0x81000000`开始，应用区`0xbe000000..0xbf000000`，只读APPRES区`0xbf000000..0xbf100000`。当前2048-context布局快照及token需70541504B，而总空闲68725188B、最大连续空闲50602016B，不能直接启用前缀快照，也不能未经规划预留最后128MiB给CPU。

性能目标先以同配置整机周期和访存统计评估，最终以板端实测确认。任何2～5token/s估算都不是当前已达成结果。
