# AI 应用（端侧 Agent + HDMI 交互）

## 模型
| 项 | 值 |
|---|---|
| 结构 | 24 层，完整权重，未替换为小模型 |
| 上下文 | 2048 token |
| 词表 | 248,320 |
| 量化 | W4 x A8，Q16 定点（RNE/饱和/下溢） |
| 加速单元 | 64 MAC lane + 8 Delta lane，G128 分组 |
| 权重文件 | HEADW4.BIN 388 MB；CPU_RAW.BIN 510 MB（其中 508.5 MB 为 tied 输入嵌入，fp16） |

## 实测（实板串口，2025.2 闭合位流 102 MHz）

| 指标 | 值 | 出处 |
|---|---|---|
| 词表加载 | 6.6 s | STAGE TOKENIZER_OPEN |
| 模型冷装载 | 540 s，1,753,611 个 512B 块，硬件 CRC 全通过、0 错误 | MODEL_COLD_STORAGE |
| 模型冷装载（惰性嵌入试验） | 328 s，762,890 块 | 同上 |
| 直连对话首字 TTFT | **11.1 s** | ttft_ticks=0x439ec445 |
| 解码速度（全量装载） | **2.03 s/token，0.492 tok/s** | decode_interval_ticks |
| 端到端吞吐 | 0.380 tok/s（19 token / 49.9 s） | task_millitokens_per_second |
| 解码速度（惰性嵌入） | 3.00 s/token，0.333 tok/s | — |
| 单次回答上限 | 96 token | AGENT_DIRECT_MAX_TOKENS |

## 关键问题与结论（实测得出）

### 1. 直连对话"不吐字"的真正原因
一次成功运行（rc=0）：19 token / **76 字节** = 每 token 约 4 字节，说明模型输出的是**中文**；而 HDMI 字库只有 ASCII 128 字模（2 KB），中文码点被映射为占位符，因此屏幕上看不到内容。

处理：在直连对话提示词前加入一行系统提示 `Answer in English, one short sentence.`（约 15 token），不改模型权重。当前板上固件即为该版本。

### 2. 惰性嵌入加载的取舍
把 508.5 MB 的 tied 输入嵌入改为按行读取，装载 540 s → 328 s，但随机偏移读 2 KB 会重走 FAT 链，**每 token 多约 1 s**（2.03 → 3.00 s/token）。因此演示固件关闭该选项。

### 3. TCM 容量已到上限
64 KiB TCM 只剩不到 1 KiB 余量。尝试在屏幕上追加 TTFT/tok-s 行会触发链接断言 `Agent TCM overflow`，因此该功能未加入当前固件；指标通过串口 `AGENT_GENERATION_STATS` 输出。

## 交互形态
- 输出：MS7210 HDMI 720x480p，80x30 字符单元，全英文 UI
- 输入：板载 KEY1–KEY4（低有效）、屏幕虚拟键盘、UART 键盘
- 功能：任务菜单、工具调用回执、CoreMark 跑分页、装载进度条、直连对话

## 未完成 / 如实说明
- 识别率与任务成功率**未测量**，无任何量化数据
- 模型标注 quality_approved=0，不声称与 BF16 等价
- 中文输出未做字库支持（当前以英文提示规避）
