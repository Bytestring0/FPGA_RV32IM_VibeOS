# 位流、固件与烧录

> **重要：PDS 不接受中文路径。** 烧录时请使用英文路径 `D:\FPGA\RiscV\bitstream\ag_npu_board_top.sbit`（本目录已同步一份副本用于归档）。
> 若直接选本目录（`提交材料/...`）下的位流，PDS 会报 `Directory name shall contain characters only from [0-9][a-z][A-Z]|[_]|[-]|[.]|[ ]|[@]`。

## 目录

| 路径 | 内容 |
|---|---|
| ag_npu_board_top.sbit | 已闭合位流（102 MHz，All Constraints Met，hot102） |
| firmware_direct_chat.bin | 配套 TCM 固件（直连对话版，板上已验证启动） |
| 烧录资产/stage1 | APPRES.BIN 63,808 B + FONT.BIN 1,048,576 B |
| 烧录资产/stage2 | APPRES.BIN 92,832 B + FONT.BIN 2,048 B + firmware.bin |
| 工具/board_agent_uart.ps1 | UART 资源上传脚本 |

## 烧录顺序（不可颠倒）

1. Fabric Configuration GUI 烧位流：JTAG 模式 → 扫描到 PG2L100H（ID 0x00602899）→ 选 `D:\FPGA\RiscV\bitstream\ag_npu_board_top.sbit` → Program。
2. 位流里的引导程序只等待串口资源；此时 HDMI 无界面、串口打印 ASSET_READY。
3. 传固件（不带 -Bitstream）：

```
& ".\工具\board_agent_uart.ps1" -Port COM12 `
  -Assets ".\烧录资产\stage1" `
  -HandoffAssets ".\烧录资产\stage2" `
  -UiBootOnly -PacketBytes 256
```

4. 约 4–5 分钟后打印 BOARD_UI_BOOT_PASS，HDMI 进入主菜单。

## 说明
- 串口固定 115200 8N1，COM12。
- 第一级必须传满 1 MB：那是位流内引导固件写死的字库尺寸，不是 UI 实际使用的 2 KB 字模。
- 突发版位流在实板重载下挂死，不要用于演示或提交。
