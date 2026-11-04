# 2026-11-03 NPU／Agent 优化检查点

当前结论：远程 NPU 与 Agent 分支已合并；已完成若干可复现的 RTL 和运行时加速。**尚未达到可上板验收状态，没有 >10 tokens/s 或162 MHz时序闭合证据，模型任务质量未批准。**

## 分支与资产

- 主线由 f7f87ea 快进合入340bfa6（NPU），再合并549b3cd（Agent），合并提交7497360。用户原有文档、忽略规则和子模块改动保留；原始 tracked 改动另有 stash 备份。
- 模型输入在工程 Data/：HEADW4.BIN、CPU_RAW.BIN、HDTENSOR.JSN、TOKEN.BIN。四项尺寸与固定SHA256在Agent构建时通过。大文件已加入忽略，不复制、不提交权重。
- 硬件运行时使用 NPU 内嵌 integration/agent_runtime_20261101/sw。根 sw/model、sw/agent 仍保留，不能直接替换缺少 tensor-v2 async、W4 head 的硬件路径。构建报告记录差异。

## 已实现与实证

下表均为相同或明确标注的定向仿真周期；不是FPGA token/s，也不是闭合频率。

| 项目 | 合并初始／对照 | 当前记录 | 证据／限制 |
| --- | ---: | ---: | --- |
| 64lane矩阵，N16 K2048，DDR BFM延迟2 | 4393周期 | 1842周期 | 四槽有序预取、双bank激活、四group发射／退休重叠；DMA短测含16/32/64配置与故障排空 |
| 64lane矩阵，N8 K512，延迟20 | 2038周期 | 816周期 | DDR有序／无外部ID；保持写ACK后计数 |
| raw-W4归约，1024group | 33884周期／2048读 | 8组并行3936周期／320读（4组为5129） | partial／scale双流cache、三拍除127；8组两阶段保留顺序饱和累加和opcode19数值诊断 |
| Delta n128单头 | 标量452001周期 | 约62481周期 | 8列并行、q/k/v局部预载、八bank scratch、整行写回；Q16/Q24、状态二次更新及饱和计数逐项参考通过 |
| RMSNorm n1024 | 旧基线98653周期 | op8约38504、可选op22约22197周期 | 后续加入双输入cache与4位／拍精确除法；op22使用单次倒数，属于显式新数值策略 |
| 同输入投影共享激活量化 | 4671 tensor job/token | 4563 | 真实单token完整logits FNV1a32均230782938，非仅argmax一致 |
| 短范围cache维护 | 小测试SET_BITS2：11周期 | 5周期 | 仅遍历涉及set，完整tag筛选；回绕、邻居脏行、错误保留和迟到ACK覆盖 |
| 可选SRAM整机对照 | DDR读2660／写1147 | 当前生产CPU：读1408／写776 | 同一短验收，统一CPU/cache/matrix/tensor路由，连续两个tile复用测试；较早版本曾为读1350／写744，不混称当前数据 |

相关源码：[矩阵DMA](../../AI加速器/npu/system/rtl/npu_dma_top.sv)、[tensor](../../AI加速器/npu/system/rtl/npu_tensor_v2.sv)、[Cache](../../rtl/ag_l1cache.sv)、[共享SRAM](../../AI加速器/npu/system/rtl/npu_partial_sram.sv)。

## 可选64KiB partial SRAM

硬件 PARTIAL_SRAM_ENABLE 默认0，固件显式 --partial-sram／QW_PARTIAL_SRAM。控制寄存器0x10007000，ID=0x50535231、容量65536。物理窗口base必须64KiB对齐。CPU/cache与加速器共同经过同一物理地址路由，无私有别名；旧DDR响应先排空，SRAM回复完成前阻止后续DDR。enable/reset逐行清零，不对整个RAM数组施加复位。

模型partial缓冲按64KiB对齐，并根据实际raw输出行stride限制tile（保持16行倍数，尺度地址对齐）。软件矩阵、tensor矩阵、head验证、可选整数W4 head统一使用容量限制。当前单tokenSRAM分块：raw795、tensor5093，完整logits哈希仍230782938。相比固定最坏256行分块的raw2566，避免大量多余任务；但相比普通DDR缓冲raw265仍增加调度，**不可仅按带宽减少推断端到端加速。**

正常板端仅在启动时启用一次：所有生产者静止，清理对应CPU缓存，配置base，enable，等待初始化。**不提供热切换API。** 手动关闭／改base前，必须停止所有生产者、等待DMA/Cache/总线排空，并clean+invalidate旧和新物理窗口；总线idle本身不能证明上游L1无脏行。不能直接重映射并继续使用旧缓存。

单元覆盖：local读写、字节保留、边界、非对齐拒绝、响应背压、4笔DDR在途时disable/remap拒绝、有序回复、重启清零。整机覆盖CPU缓存写→raw写→tensor读→CPU回读及同窗连续两个tile，错误expected和跳过cache维护必须失败。整数头定向覆盖248320行，243个tile、最大1024行，stub仅生成零partial，不是模型推理质量证据。

## 真实吞吐约束

当前模型每token矩阵751894528 MAC，W4权重375947264 B，FP16尺度11748352 B。raw partial每group8 B，共5874176group；DDR缓冲路径写回再读93986816 B。18×16×128²×4 B recurrent state至少读写37748736 B。

- 普通路径最低519431168 B/token，4GB/s物理DDR理论峰值下上限7.7007 tokens/s，未含激活、KV、CPU复制、额外仲裁。
- partial SRAM消除约94MB的外部partial往返后仍至少425444352 B/token，对应理论上限约9.402 tokens/s；仍超过10tokens/s对应400MB/token的理论DDR预算约25.44MB。
- 无partial且无任何state流量的纯权重＋尺度也387695616 B/token，10 tokens/s需3.877GB/s，约峰值96.9%。仅提频或扩大乘法器无法改变这个带宽下界。
- 实际图单token调用census（启动token16，短context），以真实n/m/k/参数分组，在独立RTL BFM上用合成操作数测每shape周期并加权；共享量化、精确4位除法与8组归约后，当前SRAM分块5093次调用的tensor预算约65707201周期。162MHz是假设，tensor单项约0.4056s；10token/s的1620万周期上限要求再削减至少49507201周期（约4.06倍），未含matrix、CPU、Cache、memcpy和长context。此预算不是实板吞吐，更不是完整模型RTL仿真。

下一阶段若坚持同一模型自由生成>10 tokens/s，需要矩阵＋尺度归约融合、状态驻留／高吞吐Delta、跨算子常驻和向量执行，并进一步探索可验证的多token权重复用或更低比特候选。限定词表会改变自由生成分布，不能当作通用模型等价加速；多会话aggregate也不能充当单会话速度。

## 数值和Agent状态

旧整数图相对相同W4 body/head软件参考，单token logits相对L2约0.167，argmax220→13。可选精确倒数Norm后本样本argmax恢复220，L2约0.1818；op21自适应尺度不保证整体质量改善，组合试验仍可能选13。**全部保持quality_approved=false。** 参考也不是官方BF16模型，不能用这一个L2或token判模型可用。

真实tokenizer最短chat smoke：输入1+1=?，16个prompt token，限4个生成token观察到“1 + 1”；输出被截断，不是回答正确性／Agent工具成功证明。完整R4、真实工具回执、模型状态／错误策略保留，没有用主机替代端侧并声称板上推理。

板端增加真实采样、非stop生成token、字节、TTFT、同turn decode间隔和任务整体吞吐。默认关闭逐token UART，保留计数；人工结构前缀、im_end等不算生成。资源按真实链接地址装载并验SHA；TCM资源不再错误要求DDR APPRES。编译必须显式 --clock-hz，PDS核对Agent固件构建报告clock／hexSHA／SRAM配置，仿真绑定核对RTL哈希。

## 物理实现边界

普通优化路径曾在162MHz配置通过PDS完整Compile，报告 stream_norm162_compat_20261103。它对应当时源码，后续修改不能继承该Compile证据。限时综合 stream_norm162_synth_20261103 在120秒自动终止，未出完整资源；日志有constant probe loop warning。可选SRAM路径 sram162_current_20261103 在120秒于DFF inference附近超时，现有源码哈希匹配的 current_sram162_20261103 在60秒限额内停于DFF inference；latest_bits_sram162_20261103 是后续已回滚的CPU选择位试验，不代表现有源码；Q/K/V bank化和CPU选择位改写均未改善限时综合，不能称完整Compile通过／时序闭合。未启动多seed布局布线，没有新位流，没有上板。

遵循短测试约束：常规定向仿真几秒内，编译命令通常60／120秒上限；真实单token主机参考装载约30–50秒并限时。PDS按进程树终止超时，检查日志E:而非仅信任exit0。

## 复现入口和本机结果

工具目录 AI加速器/npu/system/tools；WSL Ubuntu-24.04，交叉前缀 /usr/bin/riscv64-unknown-elf-。输出目录须新建；不直接覆盖已有报告。

~~~powershell
wsl -d Ubuntu-24.04 -- python3 /mnt/d/FPGA/RiscV/AI加速器/npu/system/sim/run_dma_short.py --output /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/dma_new
wsl -d Ubuntu-24.04 -- python3 /mnt/d/FPGA/RiscV/AI加速器/npu/system/tools/run_tensor_short.py --output /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/tensor_new
wsl -d Ubuntu-24.04 -- python3 /mnt/d/FPGA/RiscV/AI加速器/npu/system/tools/run_tensor_system_v2.py --compiler-prefix /usr/bin/riscv64-unknown-elf- --output /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/system_new --profiles dual --ddr-outstanding 4 --matrix-lanes 64 --partial-sram --cpu-mode original --timeout-seconds 60
wsl -d Ubuntu-24.04 -- python3 /mnt/d/FPGA/RiscV/AI加速器/npu/system/tools/run_integer_head_sram.py
~~~

本机build证据：system_original_cpu_final_20261103、system_latest_bits_20261103（此版本CPU改写已回滚）、model_current_sram_20261103、tensor_budget_sram_raw8_20261103、throughput_sram_codec_guarded_20261103.json、w4_codec_probe_verified_20261103.json；历史档案：dma_stream_faults_20261103、tensor_raw8_static_20261103、tensor_div4bits_20261103、system_div4_current_20261103、system_partial_sram_init_20261103、system_sram_repeat_20261103、model_sharedact_20261103、model_sramtiles_capacity_20261103、integer_head_sram_20261103、throughput_current_20261103.json。build目录忽略，重要结论在此记录；不将旧报告hash误称当前源码全通过。

独立只读审查确认并已修复：固件clock默认值脱离硬件、SRAM可选整数W4 head tile超容量。可选整数head已通过248320词表行、243分块的synthetic partial零值容量测试；非真实模型质量测试。新增SRAM热重映射缓存契约明确。外部codex666 DeepSeek两次502未收到意见，后改为同会话单独审查子代理；其意见均由主代理核查。

## SRAM实体资源及权重分布（同日追加）

独立 SRAM 模块先后尝试256位逐字节动态写口、32个8位bank和16个16位bank。小试验证明2048×16单地址同步RAM可用1个DRM36K和零LUT，两个字节写使能也可映射；双地址“初始化清零 + 普通访问”则推断极慢并可能落LUT。前两者曾映射约12016 LUT、零DRM（部分编译在DFF推断超时）；不具备资源竞争力。最终将清零与正常写**复用一个选定地址**，读也走相同同步地址，并按16×2048×16（可字节使能）bank。PDS独立模块综合日志 build/partial_sram_physical_20261103/oneport.log 明确报告：16个GTP_DRM36K_E1、492逻辑LUT、0个分布RAM LUT，单元测试和整机短验收重跑通过。注意：该**单独模块**暴露1268个测试用IO，独立dev_map报IO不足；这是探针顶层而非板卡实际引脚，不可声称完整NPU SoC已过device map/时序。整机可选SRAM依旧默认关，物理时序未闭合。

对现有 Data/HEADW4.BIN 先校验完整文件SHA256和模型包索引，再**只对W4权重区**（不含FP16尺度）取8段、每段1MiB均匀抽样：16符号熵约3.5236bit/权重。7个常见符号覆盖79.08%，可编码的“7个3bit码＋1个逸出码追加原4bit”粗估约3.8366bit/权重；这是权重字节的约4.08%节省，不足可靠跨越10tokens/s下界。先前混入尺度、且误把8个常见值同时塞进3bit码与逸出码的估计作废。要将可选SRAM路径原始最低425444352 B/token压到4GB/s÷10=400MB/token，至少还须无损节省25444352 B/权重375947264 B，即6.77%——这仅为100%物理峰值、零仲裁/激活/CPU开销的最低条件。该W4抽样的理想熵约可节省权重11.9%，但有限块索引、DDR对齐、解码/解压缓冲及实际效率会抹去收益；熵统计不能当成已实现压缩，不能宣称达标。复现：python AI加速器/npu/system/tools/analyze_w4_entropy.py --weights Data/HEADW4.BIN --output AI加速器/npu/system/build/w4_entropy_new.json（脚本不加载或修改权重，仅校验哈希及抽样）。

独立SRAM性能改善与吞吐差距都是真实问题，当前最值得投入的是**省掉partial roundtrip的直接矩阵-尺度融合与状态读写驻留**，同时重新组织DDR burst/权重流；未实现的压缩不要加入默认模型包。PDS许可证在本机提示剩余0天，后续长物理实现须确认许可合法有效。

## 可复核的无损压缩与计算缺口

SHA256正确的HEADW4.BIN仅抽取W4重量区8MiB；前4段用于选择canonical Huffman码、后4段完全独立测评，128个64KiB分块逐块回读一致；含每块512B对齐和8B索引后**独立留出样本**比例为0.898315（八段混合比例0.892029）。将留出比例**乐观外推**到全部375947264B权重，同时假设可选64KiB SRAM完全消除partial DDR往返时，理论外部流量最低387216316 B/token，对应4GB/s峰值下10.3301 token/s的**DDR单项理论上界**。要达10token/s需DDR持续利用率至少96.80%，尚不含争用、激活、CPU、解码缓冲和长上下文。未生成完整压缩包，未实现FPGA解码，未完成压缩后的模型数值对比或整机PDS；**不能声称已超过10 token/s**。复现：python AI加速器/npu/system/tools/probe_w4_codec.py --weights Data/HEADW4.BIN --output AI加速器/npu/system/build/w4_codec_new.json。

同一5093项SRAM census上的独立RTL加权预算65707201周期/token；162MHz要达10token/s，光tensor还需再削减49507201周期（4.06倍），即使矩阵、缓存、解码和CPU都按零周期也不达标。分项：raw-W4归约24555738、Delta 17995672、conv 9482400、量化5091677周期。**即使假设raw归约和Delta两个整个算子都免费，其他tensor仍23155791周期，大于1620万的单token上限**。751894528 MAC用64个MAC理想满载也需11748352周期；若矩阵与剩余tensor不能重叠，目标总预算只剩4451648周期处理所有tensor、CPU和DDR开销。真正攻关需要矩阵＋尺度归约融合、Delta/卷积向量化、跨算子并行与减少重复搬运，再分别验证带宽、资源与数值。带宽账面压缩不能掩盖计算瓶颈。

## 当前源码配对证据（2026-11-03）

- 生产原版CPU RTL＋当前NPU＋可选SRAM的双发射短验收：`AI加速器/npu/system/build/system_original_provenance_20261103/report.json`，passed=true、无trap、matrix完成5次、DDR读1408／写776；错误预期与跳过Cache维护反向例必须失败。回归只追踪实际RTL／固件／oracle／生成器输入；不再因更新无关分析脚本失效。`--cpu-mode original`为默认，PDS也拒绝兼容性改写CPU的仿真报告。
- Agent固件：`AI加速器/npu/system/build/cooperative_qkv_bank162_20261103/report.json`，显式162 MHz、可选SRAM，编译通过，TCM静态117968B；`quality_approved=false`、`board_runs=0`。完整主机模型：`AI加速器/npu/system/build/model_current_sram_20261103/report.json`，单token logits哈希230782938，**不等于官方BF16质量验证**。
- `npu_system_pds/original_provenance_bind_20261103/report.json`完成固件SHA、时钟、SRAM配置、原始CPU仿真源码的**准备阶段绑定**，没有跑完整PDS。与现有CPU和tensor源码哈希匹配的真实PDS `current_sram162_20261103`在60秒DFF推断阶段超时，无Synth、DevMap、时序或位流；`latest_bits_sram162_20261103`仅属已回滚CPU改写试验，不混用为当前Compile。源码层Q/K/V bank化与CPU选择位改写的限时综合无收益，已按备份**逐行和报告SHA**恢复。
- 严格准备度报告：`AI加速器/npu/system/build/board_gate_verified_20261103.json`：`ready_for_board_smoke=false`。剩余：完整PNR及setup/hold、资源与queue4 CDC审计、官方BF16质量批准、SHA绑定位流。工具核对模型资产SHA＋精确norm/reduce/SRAM开关、厂商DDR IP／生成RTL／约束现值、候选位流与路由报告同源。独立审查发现的三处条件性误判已修；伪造质量权重、DDR IP、压缩全权重字节和加权周期四种反测均拒绝。工具只评估**首轮板端smoke前置条件**，永不凭主机测试宣布竞赛成绩。
- 匹配现有SRAM模型5093 tensor任务的统计：`AI加速器/npu/system/build/throughput_sram_codec_verified_20261103.json`。当前合成操作数tensor约65707201周期/token；162 MHz需至少再省49507201周期才有可能达到10tokens/s（其他阶段均假设零成本）。可选SRAM纯DDR理论上限约9.402tokens/s；未部署的Huffman**独立留出样本**＋SRAM乐观上限仅DDR单项10.330tokens/s、要求峰值96.80%；编码模块、整包等价、时序和实际带宽均未实现。

## 上板通知条件

当前仍未满足：当前源码综合／资源可容纳、162MHz或最终频点setup/hold闭合、可用模型任务质量、真实端到端速度预算成立、稳定Agent闭环及设备输出。到时再给配套位流／固件／资源哈希与短上板验收命令。竞赛正式CoreMark≥10秒和ILA是独立成绩，TCM旧分数不可混算新整机资源。
