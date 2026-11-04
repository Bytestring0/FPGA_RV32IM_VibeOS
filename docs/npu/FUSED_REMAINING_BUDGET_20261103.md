# 融合后的剩余 tensor 预算与速度边界（2026-11-03）

本次完成的是实际 shape census 加权的独立 RTL 周期预算，**不是 FPGA 实测、不是完整模型端到端吞吐，也没有承诺 10 token/s**。tensor基线配置为 scalar-raw、parallel-delta、conv8、split read cache；不使用 partial SRAM。已补测全图融合矩阵8种真实shape，并按主代理提供的317例通过、n128每头实测省8158周期的Delta II2结果作单独外推合并，未把静态节省上限当成实测。

## 实际输入与测量范围

原始输入为 [非 SRAM、共享激活量化的单 token census](../../AI加速器/npu/system/build/model_sharedact_20261103/tensor_jobs.csv)：4563 次任务。按 opcode、n、m、k、param0、param1 原样分组，未编造或缩小 shape。`--exclude-raw-reduce` 只剔除由另行验证的矩阵融合实现的 opcode19：265 次任务、5874176 个 raw group，留下 4298 次、313 个完整参数 shape。opcode21 和其他任务不会自动剔除。

**剔除的含义是换到矩阵融合路径实现，不是该工作零成本。** tensor单独预算不含融合矩阵；下文另测融合矩阵并合并。两者均不含CPU 调度、MMIO 提交、Cache 维护、memcpy、真实共享 DDR 争用或长上下文增加的 attention 任务。BFM 操作数为合成数据，使用真实 shape 和调用次数，不能证明模型质量。BFM 包含自身请求背压及响应延迟，但不模拟整个 SoC 的总线竞争。

[预算入口](../../AI加速器/npu/system/tools/run_tensor_budget.py) 增加 `--scalar-raw`、`--shared-raw-accum`、`--exclude-raw-reduce`，另支持 scalar Delta/conv 对照。`--shared-raw-accum` 在 scalar-raw 配置下无活动作用。执行开始将依赖与 census 一次复制并绑定到结果目录，从快照编译，后续并行集成不会污染本次结果；不因当前 RTL 的地址收窄等周期不变修改反复重测。

结果为 [原始预算报告](../../AI加速器/npu/system/build/tensor_budget_fused_remaining_20261103/report.json)、[逐 shape 周期](../../AI加速器/npu/system/build/tensor_budget_fused_remaining_20261103/budget.csv)、[剩余任务清单](../../AI加速器/npu/system/build/tensor_budget_fused_remaining_20261103/tensor_jobs_measured.csv) 和 [速度／流量汇总](../../AI加速器/npu/system/build/throughput_fused_remaining_20261103.json)。

## 剩余算子热点

总计 **35,505,032 周期/token**。

| 算子 | opcode | 调用次数 | 加权周期 | 占剩余 tensor |
| --- | ---: | ---: | ---: | ---: |
| Delta | 20 | 288 | 17,995,667 | 50.69% |
| 激活量化 | 15 | 97 | 5,092,139 | 14.34% |
| 当前精确 Norm 策略 | 22 | 973 | 3,867,490 | 10.89% |
| conv8 | 12 | 216 | 3,835,512 | 10.80% |
| 激活函数 | 5 | 528 | 1,942,248 | 5.47% |
| 元素运算 | 3 | 696 | 1,491,384 | 4.20% |
| 其余 | — | 1500 | 1,280,592 | 3.61% |

按真实 n/m/k 合并 param0/param1 仅用于展示；计时仍逐完整参数 shape 测量：

| opcode | n | m | k | 调用次数 | 每次周期范围 | 加权周期 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 20 | 128 | 0 | 0 | 288 | 62,477–62,485 | 17,995,667 |
| 12 | 512 | 0 | 4 | 216 | 17,757 | 3,835,512 |
| 22 | 128 | 0 | 0 | 864 | 2,828–2,830 | 2,443,968 |
| 15 | 3584 | 0 | 0 | 24 | 98,455 | 2,362,920 |
| 15 | 1024 | 0 | 0 | 49 | 28,139 | 1,378,811 |
| 15 | 2048 | 0 | 0 | 24 | 56,267 | 1,350,408 |
| 22 | 1024 | 0 | 0 | 49 | 22,198 | 1,087,702 |

只按完整参数 top12 排序会把参数各异的 288 次 Delta 分散，误以为 conv 最重；汇总脚本现在同时输出 opcode 聚合和 opcode/n/m/k 聚合。

## 可用速度的预算边界

新增 [全图融合矩阵预算入口](../../AI加速器/npu/system/tools/run_fused_matrix_budget.py) 与 [复用现有Tb的Bench](../../AI加速器/npu/system/sim/test_fused_matrix_budget.cpp)，只编译一个64lane二进制，8种shape全测且逐shape数值/状态/ACK检查通过。编译11.851秒、仿真0.822秒。结果为 [矩阵报告](../../AI加速器/npu/system/build/fused_matrix_budget_20261103/report.json)、[实际shape清单](../../AI加速器/npu/system/build/fused_matrix_budget_20261103/matrix_shapes.csv)、[逐shape测量日志](../../AI加速器/npu/system/build/fused_matrix_budget_20261103/matrix_budget.log)。

原模型目录没有raw_jobs.csv。采用真实opcode19的(n,m,calls)清单，并从完整catalog取精确K；m8/16/28在本模型分别唯一映射K1024/2048/3584，均为完整128group，未把K盲目填为m×128。187个catalog投影每token各调用一次、按4096行分块的完整Counter，与实际265个opcode19任务Counter逐项完全相同，才允许测量；若出现K歧义或Counter不一致脚本拒绝。输出head只执行**一次完整248320×1024投影**，分成60个4096行＋1个2560行任务，head共61次硬件任务、12,581,387周期，占矩阵34.55%。总实际MAC751,894,528，与模型规模吻合但不是用总权重字节粗推周期。

| 实际N | 精确K | 调用次数 | 其中head次数 | 每次融合周期 | 加权周期 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 1024 | 36 | 0 | 952 | 34,272 |
| 512 | 1024 | 12 | 0 | 26,077 | 312,924 |
| 1024 | 2048 | 24 | 0 | 93,072 | 2,233,728 |
| 1024 | 3584 | 24 | 0 | 158,831 | 3,811,944 |
| 2048 | 1024 | 36 | 0 | 103,847 | 3,738,492 |
| 2560 | 1024 | 1 | 1 | 129,767 | 129,767 |
| 3584 | 1024 | 48 | 0 | 181,607 | 8,717,136 |
| 4096 | 1024 | 84 | 60 | 207,527 | 17,432,268 |
| 合计 | — | 265 | 61 | — | **36,410,531** |

每个shape使用新的BFM状态、合成W4/A8/正FP16尺度及maximum=INT32_MAX，固定种子17；包含guard、数值诊断及最终写ACK检查。内存为隔离DDR，响应在接受后2拍、tick%5==1请求停顿。cycle_count不含MMIO配置。合成数值不代表模型实际激活数值，不证明模型质量，也不建模CPU并发争用。

矩阵36,410,531＋tensor基线35,505,032 = **71,915,563周期/token**。Delta II2单头独立回归实测省8158周期，按真实288个n128 head外推省2,349,504，剩余tensor33,155,528，合计 **69,566,059周期/token**。这是分离引擎串行预算相加和独立Delta差额外推，不是一次完整模型RTL运行；未重新全图测量II2后的每个参数shape。

| 假设时钟 | 基线矩阵＋tensor预算上界（token/s） | 加入Delta II2差额后预算上界 | II2后达2token/s的额外周期余量 |
| ---: | ---: | ---: | ---: |
| 100 MHz | 1.391 | 1.437 | -19,566,059 |
| 125 MHz | 1.738 | 1.797 | -7,066,059 |
| 150 MHz | 2.086 | 2.156 | 5,433,941 |
| 162 MHz | 2.253 | 2.329 | 11,433,941 |

时钟均为假设，不代表布局布线通过。162 MHz达到2 token/s须把未计的CPU、Cache、MMIO、搬运和额外DDR停顿压在约11.434M周期/token内；达3token/s已超支15.566M周期，尚未计CPU。125 MHz即使CPU免费也未达2token/s。当前1–2token/s仍是待实机确认的工程目标，不能保证达成；CPU周期和实现后的时钟仍待主代理合并。没有使用两个小shape加速比外推全模型。

## 同输入的融合加速证据

使用 [DMA 对照日志](../../AI加速器/npu/system/build/w4_fused/benchmark_dma_64.log)、[tensor 对照日志](../../AI加速器/npu/system/build/w4_fused/benchmark_tensor.log) 与 [对照范围说明](../../AI加速器/npu/system/build/w4_fused/benchmark.json)。操作数 W4/A8/FP16/max 相同，DDR 接受后 2 拍响应、tick%5==1 请求停顿，周期不含 MMIO。对照 tensor 参数是 `PARALLEL_RAW=1, SHARED_RAW_ACCUM=0`，不是最终 scalar-raw 配置。

| 64 lane 实际 shape | raw DMA | 独立 tensor19 | 串行基线 | 融合 | 单元加速比 |
| --- | ---: | ---: | ---: | ---: | ---: |
| n9 k4096 | 2256 | 889 | 3145 | 2048 | 1.5356× |
| n16 k2048 | 1841 | 820 | 2661 | 1704 | 1.5616× |

两组对照是独立单元仿真，并非整个模型 token 速度。先前 1004/925 周期来自 tensor BFM 接受后3拍响应，而 DMA 为接受后2拍的不一致；已将 tensor delay 设为1，统一接受后2拍响应，得到上述默认 `PARALLEL_RAW=1, SHARED_RAW_ACCUM=0` 的889/820周期，不是共享累加配置差异。另一个连续回归中的 2051 周期也有前序请求相位差，不能替换相同起始条件对照的 2048。

## 不堆资源的后续优先项

1. **保持现有 8 个 Delta 乘法器，重排输入、乘积、饱和累加流水。** [Delta II2独立回归](../../AI加速器/npu/system/build/tensor_delta_ii2_20261103/simulation.log) 已317例通过，op20 n128测得54,323周期；与主代理同shape基线62,481相比省8158，按288头外推省2,349,504。仅本文这项已测差额进入合并预算。三段n²/8循环全部理想II2/II1曾静态估算最多省3.539M/5.308M，但那不是本次实现的收益，更不能把整个17.996M Delta简单除以2/3。顺序饱和累加不能改成平衡树；继续优化必须保留scratch同步读延迟、输出依赖和数值诊断。本文不修改RTL。
2. **量化先减少打包和控制拍，复用现有 SRAM bank／除法器。** op15 真实输入元素共185344，每次两遍读取，当前约 27.47 周期/元素；全图5.092M周期。第二遍用同一个 peak，若优化除法需保持 exact RNE，可考虑单次 reciprocal＋校正复用已有乘法器，而不是增设宽除法器；这会涉及新数值等价验证。更保守的是32字节输出批量装包，当前标量 PACKREAD/PACKSAVE 对每个byte两拍：185344×2=370688拍只是相关开销上限，DDR和scratch读仍不可免费。无实现前不计收益。
3. **Norm 已经选择 op22，不再把旧 op8→op22 的历史收益计算第二次。** 当前3.867M周期是该策略的实际 shape 预算；即使整个 Norm 免费，tensor 仍31.638M周期。改变精度需要独立质量证据，当前面积优先不应盲目复制除法／平方根单元。
4. **conv8 已降到3.836M周期。** 仅扩大乘法 lane 对整个模型收益有限，先保持资源并评估DDR状态搬运及scalar累加控制；不建议为10.8%的 tensor 项重新堆8套宽 round/add。

本次没有测量 LUT、DSP 或布线利用率；面积改善只由主代理的 PDS/综合实际结果决定，不能用源码共享表达式数推断。

## 直接融合的 DDR 边界

265 个被剔除的 opcode19 共覆盖 catalog 的全部5874176 group；若融合确实消除每 group 8B 写回＋8B 再读，可去掉 **93,986,816 B/token** raw partial往返，不依赖 partial SRAM。基础流量仍至少为权重375,947,264＋尺度11,748,352＋recurrent state读写37,748,736 = **425,444,352 B/token**。未含输出、激活、CPU Cache和额外仲裁。

4 GB/s 物理DDR峰值下，该基础流量单项上界9.402 token/s；假设持续效率50%／60%／70%／80%，单项上界分别4.701／5.641／6.581／7.522。它与计算周期是不同边界，不能相加两个速度或当成实际带宽。无需为了1–2 token/s先引入压缩或再加 SRAM，先让现有融合与模型路径闭合。

## 复现

以下在 Ubuntu-24.04 WSL 内运行；输出目录换新名字，避免覆盖已有快照。

```sh
python3 /mnt/d/FPGA/RiscV/AI加速器/npu/system/tools/run_tensor_budget.py \
  --census /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/model_sharedact_20261103/tensor_jobs.csv \
  --output /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/tensor_budget_fused_remaining_new \
  --scalar-raw --exclude-raw-reduce
python3 /mnt/d/FPGA/RiscV/AI加速器/npu/system/tools/summarize_throughput.py \
  --budget /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/tensor_budget_fused_remaining_new/report.json \
  --census /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/model_sharedact_20261103/tensor_jobs.csv \
  --output /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/throughput_fused_remaining_new.json \
  --clock-hz 162000000
python3 /mnt/d/FPGA/RiscV/AI加速器/npu/system/tools/run_fused_matrix_budget.py \
  --census /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/model_sharedact_20261103/tensor_jobs.csv \
  --output /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/fused_matrix_budget_new \
  --tensor-budget /mnt/d/FPGA/RiscV/AI加速器/npu/system/build/tensor_budget_fused_remaining_20261103/report.json \
  --lanes 64 --tile-rows 4096 --clock-hz 162000000 --delta-ii2-saved-per-head 8158
```

本次使用 `run_code` 的 `cp.spawnSync` 调用完整 `C:/Program Files/WSL/wsl.exe`，环境为 `SystemRoot/WINDIR=C:/Windows`、`PATH=C:/Windows/System32;C:/Windows`、`TEMP=C:/Windows/Temp`。tensor预算编译和313 shape执行通过；最终汇总通过。追加矩阵预算只build一次，8种真实shape加权通过。不重复模型运行、修改RTL/模型/固件，也不重复PDS或driver工作。
