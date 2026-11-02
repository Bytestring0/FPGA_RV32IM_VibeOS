# 当前 CPU＋DDR＋NPU＋SD 集成源码

完整状态、限制、资源清单和接手命令见仓库根目录的 [NPU_HANDOFF.md](../../../NPU_HANDOFF.md)。

- `rtl/`：生产矩阵、tensor、SD、DDR仲裁和整机顶层。
- `sw/`：正式驱动、数值参考和当前完整Agent板端入口。
- `integration/`：当前整图模型、真实Agent后端和实际构建所用R4文本。
- `tools/validate_handoff.py`：不依赖模型大文件的集中源码/整机验收。
- `tools/build_tensor_agent_cooperative.py`：完整RV32 Agent固件与配套APPRES构建。
- `config/model_assets.json`：不入Git的大资源大小、SHA和SD短文件名。
- `pds/build_system.py`：真实PDS工程生成，可显式运行综合/布局布线。

I/D Cache与NPU直读写DDR已经实现；权重预取、整图质量批准、最新整机时序与实板吞吐仍待完成。历史或软件参考PASS不能替代这些验收。
