# PDS 2025.2 加速检查

已在PG2L100H-6FBG676上实际启动2025.2 build211867，旧许可证签出成功。用户要求不再新增子代理；本次仅主代理使用PDS自己的分区子进程。

## 已确认可用

- 自动分区并行综合：`synthesize -ads -resource_sharing FALSE -automatic_partition_block TRUE -max_parallel_jobs 6`。实际工程选项为TRUE/6；日志出现Multi-run，进程列表确有6个partition_blocks综合子进程，各分区时序分析日志亦报告最多4个process。
- 同冻结设计Compile阶段：2022.2-SP6.1约120秒；2025.2约69秒，约1.74倍。这是单次阶段比较，非整轮实现加速承诺。
- `pnr -route_mode fast` / `route -route_mode fast`：新版接受fast/normal/performance模式。官方手册说明fast运行最快但可能牺牲时序质量，最终仍必须验证setup/hold。
- 多种子并行：`-gplace_times`、`-parallel`，布线`-groute_times`、`-route_parallel`。`-pick_first_timing_convergence_seed TRUE`在首个收敛种子后终止其余，可避免等待所有种子。当前未开启多种子，只跑单条fast流程，不扩大重复工作。用户已明确后续全部切到2025.2，所有2022实现与JTAG服务均已停止，仅保留历史数据库。
- 全局GUI General有Max Threads与Maximum Number of Parallel Jobs；单条route线程数参数`number_of_router_thread`实测已废弃、设置被忽略。不能声称它可令PG2L单次布线使用全部22核。新版实际run_route.log已明确报告route与Timing均启用最多4个process，现场布线子进程有18个线程，真正多进程布线已经生效。

## 已排除的捷径

- 2025读2022布局ADF：明确DB-0064格式不兼容，需要新版数据库。
- 旧syn.vm直接作为ADS后端输入：当前工程明确拒绝Flow-0150；未继续反复改扩展名绕过。
- 因此新版候选使用原封不动冻结的RTL、firmware.hex和board.fdc进行一次新版Compile/并行Synthesize/Map/Fast PnR，而非混用数据库。旧版正余量布局及正在运行的布线保留作保底。

## 运行位置

`npu_system_pds/npu_fast81_pds2025_20261103/accelerated.tcl` (81 MHz诊断)，最终CDC修正54 MHz构建为`npu_system_pds/npu_release54_final_20261103`。

`accelerated.log`为真实日志，`accelerated_report.json`记录总流程状态。当前不能仅凭Compile变快就宣布布线变快，需等待实际资源、详细布线和时序报告。

设计保持64lane矩阵NPU、8lane tensor、64KiB TCM、I/D Cache、四事务DDR、81MHz约束。旧版综合40266LUT/85DRM/133APM；新版81MHz诊断综合38836 LUT（58.31%）、93DRM（60%）、123APM（51.25%），通过器件映射；仅55元件启动口分析表明54MHz最终版本资源也可容纳。正确排程需要区别完整54MHz系统综合与81MHz低DRM性能候选，分别绑定对应固件和仿真。新版独立Route必须完成且内部setup/hold通过后才可下载。
