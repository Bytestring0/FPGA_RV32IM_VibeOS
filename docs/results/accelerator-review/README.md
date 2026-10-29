# 团队加速器审查复现

对象：Tigrisz/LLM-Accelerator，提交 `cd06ae6e9d820bca3bb5d22b8eaf91e32dec43ec`。本轮没有修改或推送上游仓库。

WSL Ubuntu-24.04、Verilator 5.020、GCC 13.2；Python 独立环境安装 NumPy 2.2.6。执行：

```sh
make test-compute test-system-control test-integration test-integration16 \
  PYTHON=/tmp/qinglong-llm-review-venv/bin/python
```

[regression.log](regression.log) 保存完整输出：16/32 路核心及相应集成测试通过；Python 87 项中 5 项跳过，82 项通过。这不是整个模型推理或完整上板测试。

[contract.log](contract.log) 来自 [审查探针](../../../tests/accelerator_v1_contract_probe.sv)，复现运行中配置被改写、reader 错误后无法仅靠 clear 恢复、部分字节写覆盖整个寄存器三个现有限制。该探针验证的是上游当前行为，不能用作修复后协议的验收标准。

分析与后续改造方案见 [模型加速器整合与浏览器方案](../../模型加速器整合与浏览器方案.md)。
