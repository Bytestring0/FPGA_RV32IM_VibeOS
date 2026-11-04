import pathlib,shutil
B=pathlib.Path('/mnt/d/FPGA/RiscV/提交材料/10_全部源码')
for d in ('tools','pds'):
    p=B/d
    if p.exists(): shutil.rmtree(p); print('removed',d)
md=B/'README.md'
md.write_text('''# 全部源码（最小可复现集）

只包含运行与复现所必需的源码，不含任何构建辅助脚本、中间产物或工程生成物。

| 目录 | 内容 | 文件数 |
|---|---|---|
| rtl/ | CPU / SoC / Cache / BTB / NPU / DDR 桥的 SystemVerilog | 14 |
| fpga/board/ | 板级顶层、时钟、DDR 顶层与约束 .fdc | 6 |
| firmware/system_sw/ | 启动、SD/FAT、资源加载、HDMI 文本、UI、跑分页 | 55 |
| firmware/model/ | 模型加载、量化、Q16 定点、张量 ABI、采样、分词 | 47 |
| firmware/agent/ | Agent 循环、工具协议、工具归一化 | 11 |
| coremark/third_party/ | CoreMark v1.0 官方核心（未修改） | 8 |
| coremark/port/ | core_portme、ee_printf、start.S、link.ld | 5 |

## 关键实现位置

| 功能 | 文件 |
|---|---|
| 五级双发射流水线、分支预测接入 | rtl/rv32_core.sv |
| 2 路组相联 I/D Cache | rtl/ag_l1cache.sv |
| BTB + 返回栈 | rtl/ag_btb.sv |
| CSR / 异常 / 中断 | rtl/rv32_csr.sv |
| DDR 异步桥与读突发合并 | rtl/ag_hmic_bridge.sv、rtl/ag_hmic_read_burst.sv |
| NPU 张量数据通路 | rtl/npu_tensor_v2.sv、rtl/npu_delta_vectors.sv |
| 模型加载与量化 | firmware/model/qw_load.c、qw_forward.c |
| 工具调用协议 | firmware/agent/qw_agent_protocol_backend.c |
| HDMI 菜单 / 键盘 / 跑分页 | firmware/system_sw/ag_agent_ui.c、ag_coremark_ui.c |

## 配套产物

- 位流与固件镜像：08_位流与镜像/
- ILA 波形与定量分析：06_ILA波形/
- CoreMark 结果：04_CoreMark/
- 时序与资源：05_时序与资源/

模型权重（388 MB W4 + 510 MB CPU 侧嵌入）不在此目录，随固件与 SD 卡提供。
''',encoding='utf-8')
tot=sum(1 for f in B.rglob('*') if f.is_file())
print('TOTAL',tot,'files')
