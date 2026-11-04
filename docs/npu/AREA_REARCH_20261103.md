# 2026-11-03 低资源架构重构

目标：保留真实矩阵/张量硬件加速，大幅降低资源并留布局布线余量。当前正在真实整机综合，未宣称PNR、板测吞吐或模型质量已完成。

## 已实现

- tensor：用显式输入选择将各微码状态的标量舍入、饱和、累加复用为一套97位舍入+64位加法；八路Delta各自一套66位舍入、64位加法及32位clip跨状态复用。并行RAW/Delta关闭时，对每个对应状态施加常量门控，保证综合真正剪除，而不只改变入口状态。
- 地址运算：i/j16位、ns/pc/returnpc15位、no/softbase/best13位、t1位；Delta地址乘8×8、其他乘数按已验证维度截位。Delta移位8/16/24/30采用常量切片选择，避免通用桶形移位。
- TCM：偶奇双口bank保留128KiB容量、双指令取指及并发数据访问。独立真实PDS：96→64 DRM，92 LUT/35FF。
- DDR四事务桥：payload从带异步复位、多次清零的寄存器阵列改为无reset单写memory，响应valid独立保持错误/写响应为零语义。独立PDS：4133→1014 LUT，2438→150FF；跨域FIFO和Gray对象保持。
- 矩阵融合：按row预读最多三行FP16尺度，单11×32系数乘法+单32×32部分和乘法，II=1精确opcode19归约。原raw-v1兼容，mode1输出连续Q16，省掉DDR tagged partial及独立tensor归约。生产候选关闭partial SRAM，额外节省16 DRM。
- 卷积：复用八个Delta乘法器及现有cache/scratch，n1024 k4 87782→35511周期、DDR读8192→1024；逐tap升序饱和与诊断保持。

## 验证与性能

- tensor_narrow_addr_20261103：315例通过，完整输出/状态/数值诊断对参考匹配；未改变算子周期。后续 tensor_delta_ii2_20261103：317例通过，新增两种饱和后抵消顺序验证。只重排两段现有八路MAC内积循环为II2，无新增寄存器、乘法器或存储；n128 op13/op20从62481降到54323周期（省13.06%，1.150倍），DDR读2096次不变。
- TCM：10000周期旧RAM逐周期等价，生产SoC smoke通过；桥：四种异步clock ratio、四在途、reset abort、错误零payload及legacy通过。
- w4_fused：16/32/64lane及旧raw-v1短回归通过，全部31743个正有限FP16尺度、极值、RNE ties，连续4096组II1压力通过。
- 统一DDR请求accept+2拍、同W4/A8/FP16/max：64lane n9 k4096，旧2256+889=3145周期，融合2048，1.5356倍；n16 k2048，旧1841+820=2661，融合1704，1.5616倍。DDR写74/66→2，比较不含MMIO配置，非板测。证据：AI加速器/npu/system/build/w4_fused/benchmark.json。
- 最终稳定系统 system_fused_final_20261103a/report.json：passed=true，source_changes_during_run为空。生产原版RV32双发射+cache+64lane+queue4，scalar-raw/shared-raw-accum、融合开启、无partial SRAM；连续两次3×129任务153/151周期，Q16 CPU回读、2B对齐scale、partial poison保持、行ACK计数均通过；随后27tensor、总5次matrix完成、无trap、peak_pending4。错误期望/错误融合输出均exit10，跳过cache维护exit43 BadScale，反向验证均正确拒绝。
- 非SRAM模型census共4563任务，融合移除265次独立opcode19后，剩余4298次tensor调用，独立BFM加权35505032周期/token。此预算不含融合矩阵、CPU、cache维护、长上下文和物理频率，不能当端到端token/s。

## RV32对硬件的直接加速证据

生产RV32＋Cache＋queue4仿真中，以相同16×2048 W4/A8/FP16输入比较GCC -O2标量完整算子与生产融合驱动，使用真实cycle CSR，两路径起始均clean＋invalidate冷输入，所有16行Q16逐项相等。双发射：软件651598/651441周期，对应硬件完整任务4586/4530周期，142.08～143.81倍；单发射：809109/809129对4892/4776周期，165.39～169.42倍。硬件计时含cache发布/获取、MMIO、poll和诊断；CPU含权重解包/读取、乘加、系数除法及归约。此为具体C基线下单算子仿真，不能外推完整模型token/s或板测。证据 build/rv32_fused_benchmark/report.json。

## 硬件专用固件

显式 `--tensor-only` 编译配置仅保留硬件tensor模型路径，缺少tensor设备、缺少execute或误选软件raw模式时返回FORMAT；UNKNOWN租约优先级保留。主机/旧配置仍含原完整FP参考。真实融合Agent构建 `cooperative_fused_tensoronly162_20261103` 通过：text112440→82856B，静态TCM119796→89964B（减少24.9%），保留8KiB栈后的额外余量3084→32916B。完整R4保留；仅自动回收不再引用的FP兼容图与LUT，未删tensor模型/头/词表。正式交付使用 cooperative_fused_tensoronly_precise162_20261103，同样89964B静态TCM，显式precise-norm/op22、融合mode1/op19、tensor-only、无partial SRAM；模型和tokenizer四项资产在构建时验证通过。其数值策略与已通过的真实主机单token检查和算子预算一致；质量批准与实板记录仍false。

## 首次运行入口

新增 `build_tensor_agent_cooperative.py --model-smoke-tokens 2`：模型资源装载后，直接用真实NPU执行两个完整token，包括全部层与248320词表输出头；记录每token周期、采样结果与tokenizer片段，完成后退出。不缩小模型或词表，也不代入答案。该选项省去首次调通时长Agent提示prefill，正常Agent模式保留。

已通过构建 model_npu_run54_20261103：融合mode1、tensor-only、precise-norm、DDR资源，静态TCM52640B。54MHz为首次实际运行目标，完整PNR已启动 npu_model_run54_final_20261103，频率和位流仍需实现结果确认。

## 完整模型已实际通过生产NPU RTL

`full_model_npu_rtl_20261103/rtl_report.json` passed=true：真实HEADW4/CPU_RAW模型，2048上下文配置，全24层和248320词表完整head，实际265次融合矩阵、4298次tensor任务均由生产RTL执行；矩阵36410775周期，tensor30445741周期，总总线67108499周期。输出token220，无饱和；完整248320项logits与此前同数值策略C参考文件逐字节相等。主机仅调度图和提供DDR BFM，基线FP分支仍为软件比较；这不是RV32整图仿真、PHY/实板吞吐或质量批准。

## 当前物理状态

旧scalar_area整机127737/66600 LUT、155/155 DRM，tensor独占104139 LUT，原设计不可容纳。真实综合 area_shared_alu_20261103：107957 LUT、117 DRM、140 APM；后续 area_narrow_map_20261103：88574 LUT、117 DRM、133 APM，device map因LUT88570>66600明确失败，综合估频56.8202MHz、162MHz约束setup−11.425ns。地址收窄与存储重构确有收益，但尚不足容纳；不能声称已可布线。最新综合网表实证：Delta V的8个128:1动态读mux各约2080 LUT；大量cache_data的256→32输入选择最终接到delta_q/k/v每个字的D口，是八路动态写口被逐字展开（例如N127261→delta_v[127][0].D），不是普通readword任务复制。已改为8个16深固定lane、单写口的Q/K/V bank；独立tensor真实综合17094 LUT，317项数值回归通过。为断开实际运行的长路径，后续增加标量舍入输入/结果流水，317例和整机验收均通过，完整模型RTL输出逐字节一致。最新整机 npu_model_run54_final_20261103 综合39613 LUT（59.48%）、19564寄存器、117 DRM（75.48%）、133 APM（55.42%），器件映射通过，布局布线进行中。按用户最新要求，先以资源可容纳并完成运行闭合为优先，不再为70%占用目标继续扩展优化；真实setup/hold和位流以最终PNR报告为准。

用户已连接USB Cable II JTAG、CP210x COM12，FAT32 SD曾核验完整模型五个文件与APPRES.BIN。USB串口/JTAG可用，但最新54MHz PDS2025物理报告仍显示setup/UI-CDR和DDR PHY hold违例且没有位流；没有进行不合格配置下载或板端运行。完整模型已由生产matrix/tensor RTL协同仿真完成、RV32融合单算子加速对照通过，质量认证和板端token/s仍未验证。
