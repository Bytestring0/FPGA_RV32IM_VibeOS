# PDS 实现路径与当前物理阻塞（2026-11-03）

这是物理构建脚本子任务的检查结果；不代表新 RTL 已完成实现或上板。

## 同日真实面积改造结果（19:22后追加）

两份长综合已完成并收集，未因900秒默认值丢失正常推进的工具任务；延长等待只暂停Python超时监控，PDS持续运行，结束后监控均正常恢复。

| 冻结候选 | LUT | 寄存器 | DRM36K | APM | 综合cpu_clk估频/Setup |
| --- | ---: | ---: | ---: | ---: | --- |
| area_shared_alu_20261103 | 107957 (162.10%) | 31444 | 117 (75.48%) | 140 | 52.0032MHz / -13.055ns |
| area_narrow_map_20261103 | 88574 (132.99%) | 31394 | 117 (75.48%) | 133 | 56.8202MHz / -11.425ns |

窄地址相比共享ALU整机减少19383 LUT；tensor由85824降到66017 LUT。其余窄地址分项CPU SoC9930LUT/82.5DRM，DMA5713LUT/70APM/8DRM，DDR桥1029LUT；tensor59APM/26DRM，零LUTRAM。此前TCM释放32DRM和关闭partial SRAM后，工具能将tensor全部scratch及ROM映射到DRM，不必盲目加RAM属性。

窄地址候选实际dev_map报88570/66600 LUT、DRC-0018不足，真实器件映射失败。不能启动正式PnR，也不能将估频当时序闭合。该快照早于最终DeltaII2，使用旧fused DDR固件；它是面积对照，并非最终正式数值/上板候选。

共享版scalar_rounder593LUT，八lane rounder合计2116LUT，其余tensor逻辑仍巨大。共享版最长路径pc[3]经Delta rounder低mask高fanout至APM；窄地址版最长路径pc[8]经scalar rounder guard/sticky/lowmask至sat_pending[0]。正式下一阶段需要继续压微码解码、操作数选择与诊断累计逻辑，不能仅从rounder小模块面积推断整个问题。

真实报告：npu_system_pds/area_shared_alu_20261103/synthesize/ag_npu_board_top.snr及area_result.json；npu_system_pds/area_narrow_map_20261103/synthesize/ag_npu_board_top.snr、device_map/run.log及area_result.json。首份冻结快照仅为PDS兼容给融合hex ternary补空格括号；未改生产RTL。原gated_raw试验经授权停止，其不完整snr不能作为资源证据。

正式最终功能报告为build/system_fused_final_20261103a/report.json；正式固件应选build/cooperative_fused_tensoronly_precise162_20261103/firmware.hex（由主线程验证），无partial SRAM，precision Norm22；待面积真正合适再使用该最终源码和固件绑定做PnR。

## LUT根因的网表实证

对窄地址版实际综合网表一次分组并追踪直接DFF消费者，发现384个同构QKV写数据根，每个112个LUT，分别驱动delta_q/delta_k/delta_v的128个32位寄存器。三组各14336、总43008 LUT，占该版本tensor66017 LUT的65.15%。例如N127261[0]在实际VM1611432行接delta_v[127][0]的D口；它的输入选择链来自cache_data不同32位slice。普通readword结果rv的根N106176只有160LUT，不能把43008归因为readword重复。动态delta_[qkv][i+lane]多写口应转固定lane单写bank，须由定向数值和新物理综合确认收益。

完整归因：npu_system_pds/area_narrow_map_20261103/tensor_netlist_attribution.json、tensor_register_mux_attribution.json。分析脚本位于system/pds，不运行额外综合。

已准备独立synth_tensor_area.py：真实npu_tensor_v2为top，直接约束clk，默认RAW0/DELTA1/CONV1/SHARED1，仅Compile＋Synth，无dummyCPU、DDRPHY、DevMap或板测声明。等待diag/stripe通过后运行，不读取半写RTL。独立top端口无板级IO时序，综合估频只用于定位内部关键路径，不能代替整机PnR。

## 当前真实结果

- 真实器件 PG2L100H-6FBG676：66600 LUT、133200 寄存器、155 DRM36K、240 APM。
- `npu_system_pds/pnr162_sram_cdc6_20261103/device_map/run.log:1152` 明确记录 LUT 135298/66600，接着 DRC-0018；没有进入可用布局布线。
- 标量 raw＋标量 Delta 的 `synth162_scalar_area_20261103` 完整综合完成，仍为127737 LUT（191.80%）、33535寄存器、155 DRM（100%）、134 APM（55.83%）；cpu_clk 162 MHz综合估计 setup -14.954ns。
- 上述标量版本的 tensor 独占104139 LUT、19129寄存器、66 APM、16 DRM；CPU SoC 9659 LUT/114.5 DRM，DDR桥4133 LUT，partial SRAM615 LUT/16 DRM。瓶颈在tensor展开的宽数值运算，单独减少MAC lane或降低时钟不能解决面积。
- `resource-sharing` 试验134306 LUT，tensor110688 LUT，比标量基线更差。
- 用户要求留出布线余量；整机LUT先瞄准70%即46620，tensor约需降到23000 LUT。资源目标是实现选择依据，不能当成当前实测结果。完整模型TCM仍需128KiB。若融合矩阵尺度路径不再需要partial SRAM，关闭它可省16DRM，减少目前100%占用压力。

## 已核实工具和连接

- Windows Python：`C:\Users\Au224\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe`。
- PDS：`D:\App\PDS_2022.2-SP6.1\bin\pds_shell.exe`，版本2022.2-SP6.1 build138931。旧脚本默认 `D:\pango\PDS_2022.2-SP6.4` 不适用本机。
- PDS合法许可证现有文件到期字段为2026-11-03；18:43之后一次真实pds_shell调用已经成功 `License checkout: fabric_shell`。工具随后因Unicode脚本路径乱码而报找不到文件，不能把这次当完整架构编译，但它证明当时正常签出未被到期拒绝。未改许可证、系统时间或许可证校验。跨过有效期后必须以工具真实结果为准。
- `pds_shell -help` exit0。保留ASCII输出根 `D:\FPGA\RiscV\npu_system_pds`，不要直接将含中文源目录传给PDS的 `-file`。Python可读取中文源码并合并为ASCII运行目录。
- 本轮进程检查没有现存pds/python物理编译进程，只有wslservice。板端串口仅COM3/COM4标准蓝牙串口；PresentOnly设备没有UART/FTDI/JTAG/CH340/CP210匹配。不能声称已连接或可烧录开发板。

## 构建脚本修复

`AI加速器/npu/system/pds/build_system.py`：

1. PDS安装自动发现，许可读取顺序为进程环境、用户注册表、机器注册表、安装目录唯一lic；补足Windows系统环境与工具PATH。
2. 修复 `shared_raw_accum` 已被仿真配置检查使用但CLI缺失且未传入板顶层的问题，新增 `--shared-raw-accum` 并记录对应参数。
3. 长任务不再默认60秒终止：Compile300秒、Synth1800秒、Map2700秒、PNR为路由action预算加1800秒；显式超时最多10800秒。运行时记录PID和耗时，真实超时才终止进程树。外部任务管理应使用后台job，不能再包一层60秒kill。
4. PNR必须存在实际route DB与新timing报告，且All Constraints Met，失败不得passed；区分internal_timing_closed与未验证的SD外部时序/板测。
5. 增加 `--bitstream`：仅PNR内部时序通过后生成sbit并记录实际产物；不自动烧板。原独立finish脚本同步正确工具环境与日志E:判错；verify脚本也拒绝不满足setup/hold的结果。
6. 所有 `system/rtl/*.sv` 自动纳入源表，新融合模块不需要手工维护PDS文件列表。

只执行Python语法解析与CLI入口检查，均通过；未启动完整PDS，也没有额外模型/hash循环。

## 主线程可直接调用的面积综合

以下是匹配已有162MHz/partial SRAM固件的面积探索命令；新固件和SRAM开关必须保持一致。它不传旧仿真报告，避免将面积探索伪装成正式数值验收。

```powershell
& "C:\Users\Au224\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe" "AI加速器/npu/system/pds/build_system.py" --run area_shared_20261103a --firmware "AI加速器/npu/system/build/cooperative_qkv_bank162_20261103/firmware.hex" --ip "build/ag-ddr-ip" --pds "D:\App\PDS_2022.2-SP6.1" --clock 162 --tcm-kib 128 --tensor-v2 --npu-lanes 64 --ddr-outstanding 4 --partial-sram --shared-raw-accum --mode synthesis --command-timeout-seconds 900 --build
```

推荐先完成tensor面积压缩和融合路径定向数值检查，跑一次足够预算的综合，LUT合适再Map/PNR。不要用当前162MHz参数宣称闭合。面积收敛后先以81或102MHz尝试真实布线（须重建对应时钟固件），再根据最长真实路径提频；当前162MHz综合严重负余量不适合直接多seed硬跑。最终使用 `--mode pnr --pnr-timeout-minutes 40 --bitstream --simulation-report <匹配配置的生产CPU验收报告>`，采集实际setup/hold、资源、位流和板上模型速度后才能宣布整机闭合。
