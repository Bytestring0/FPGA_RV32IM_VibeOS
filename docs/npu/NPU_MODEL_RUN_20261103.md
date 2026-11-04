# 完整模型 NPU 首次运行

当前生产路径使用64lane矩阵NPU、8lane张量执行、原生产RV32双发射CPU、I/D Cache和四事务DDR。模型是现有完整W4模型，所有24层与248320词表输出头均保留。

## 当前已完成

- 完整模型真实权重经生产矩阵/tensor RTL完成1个token：265个融合矩阵任务＋4298个tensor任务。全部248320项logits与同数值策略C参考文件逐字节相等，输出token220，无饱和。证据：AI加速器/npu/system/build/full_model_npu_rtl_20261103/rtl_report.json及model.log。
- 上述协同验证由主机调度图、RTL执行数值运算，不含RV32/Cache/DDR PHY；板端速度仍需实际运行。实际生产NPU处理265次融合矩阵和4298次tensor，隔离总线67108499周期。完整logits与同策略C参考逐字节一致。
- 必要时序修复仅为标量舍入输入/结果加流水，317项数值回归及生产CPU整机验收通过，完整模型RTL已覆盖这版。
- 生产RV32＋Cache＋NPU整机短验收system_pipeline_run_20261103通过，包含缓存一致性、重复融合任务和错误反例。
- 2025.2修正CDC方向的54MHz版本：38,592/66,600 LUT（57.95%）、93/155 DRM、123/240 APM。完整布线已完成但 `Design Summary : Some Constraints Violated`；最终route-only报告 `npu_release54_routefix_20261103` 显示WNS CPU-CPU -5.657ns、DDR UI-CPU -7.449ns、CPU-DDR UI -2.652ns，DDR PHY hold仍违反，故未生成位流。禁止下载未闭合构建。最新可用速度候选为54MHz、64KiB TCM，整机40266 LUT（60.46%）、85/155 DRM（54.84%）、133/240 APM；npu_fast81_margin_20261103正在完整布局布线，54MHz旧任务已停止。

## 配套运行文件

最终闭合尝试固件：AI加速器/npu/system/build/model_npu_final54_20261103，54MHz、64KiB TCM、静态53952B，精确Norm、融合矩阵、tensor-only及多块SD。已构建，但由于配套实现时序未闭合，尚不能下载。81MHz版本AI加速器/npu/system/build/model_npu_fast81_20261103只属诊断候选。

- firmware.hex：已经嵌入相应物理工程TCM，无需另向SD复制。
- APPRES.BIN：8706字节的配套常量资源。
- report.json：具体固件配置和装载布局。

在FAT32 SD根目录准备以下文件，内容使用工程Data中的现有完整资产，不重新量化、不压缩、不裁词表：

| SD文件名 | 工程来源 |
| --- | --- |
| HEADW4.BIN | Data/HEADW4.BIN |
| CPU_RAW.BIN | Data/CPU_RAW.BIN |
| HDTENSOR.JSN | Data/HDTENSOR.JSN |
| TOKEN.BIN | Data/TOKEN.BIN |
| APPRES.BIN | 对应固件构建目录的APPRES.BIN |

无需格式化已有SD。确认选择的是目标SD后复制上述文件；本轮没有向任何外部磁盘写数据。

## 下载和采集

位流完整生成且报告内部setup/hold通过后，连接板卡JTAG、USB UART和SD，115200 8N1。执行：

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts/run_npu_model.ps1 -Port COM实际端口
~~~

脚本使用已完成工程中配套位流，仅写易失FPGA配置RAM，采集结果保存在相同物理目录的board_uart.log、board_program.log、board_run.json。不写Flash。SD冷装载时间单列。

成功启动后应依次看到DDR就绪、SD/模型装载，然后实际生成：

~~~text
NPU_MODEL_TOKEN step=0 sampled=... decode_ticks=0x... clock_hz=0x...
NPU_MODEL_TOKEN step=1 sampled=... decode_ticks=0x... clock_hz=0x...
NPU_MODEL_COMPLETE tokens=0x... total_ticks=0x... clock_hz=0x...
CANDIDATE_EXIT rc=0
~~~

采样token来自实际模型输出，文本piece由真实tokenizer解码。该模式直接运行两个完整decode，避免首次调通前等待长Agent提示prefill；正常Agent入口与工具流程保留，构建时省略--model-smoke-tokens即可；同频54MHz正常Agent固件已构建通过，目录AI加速器/npu/system/build/agent_npu_run54_20261103，静态TCM89964B。该固件需要相应TCM初始化位流，不能把Agent HEX当作SD模型资源直接替换。

实际速度用clock_hz/decode_ticks计算；冷装载、主机仿真耗时、独立算子周期不混入板端token/s。模型任务质量尚未批准。

用户已连接FPGA。实际硬件为USB Cable II、CP210x UART COM12；F:/ FAT32卡曾只读验证HEADW4.BIN、CPU_RAW.BIN、HDTENSOR.JSN、TOKEN.BIN、APPRES.BIN大小和SHA匹配，且剩余空间足够。当前SD卷此刻未挂载。最终阻塞是新版布局setup/hold不满足且需解决DDR PHY与FIFO跨域路径；未生成位流、未烧写、未运行板端模型，也没有token/s结果。
