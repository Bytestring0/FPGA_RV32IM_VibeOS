# 102 MHz 闭合 DDR/Cache SoC 实板验证

日期：2026-10-29。MES2L676-100HP / PG2L100H-6IFBG676；FTDI USB Cable II，JTAG ID `0x00602899`；底板 CP210x 为 COM12、115200、8N1。

本次测试的是 [offline-ddr-cache-102mhz](../offline-ddr-cache-102mhz/README.md) 中冻结的改名前位流：CPU 102 MHz、DDR UI 125 MHz、DDR3 1000 MT/s、I/D Cache 各 16 KiB。已有最终时序为 All Constraints Met：CPU setup +0.056 ns，slow/fast hold +0.136/+0.099 ns。实测证据不自动转移给重命名后重新实现的 AG 位流。

## 实测结果

两次重新下载到易失 FPGA 配置 RAM，均收到完整自检 PASS：

```text
Qinglong DDR3 smoke CPU=06146580
DDR calibrated
DDR PASS data16KiB byte halfword address-lines edge execute
CPU MULH PASS signed mixed unsigned edges
CPU MINMAX PASS signed unsigned equal edges
CACHE I_HIT=00000204 D_HIT=00001efd WB=00000231 IBUF_HIT=000005ce
CACHE_WORK_CYCLES=000010f0
CACHE PASS DDR code warm-hit self-modify maintenance
```

第二次在收到完整串口输出后执行 `cfg_verify -device_index 0`，下载和配置校验均成功，状态寄存器 `0x081ffca0`。首次快照只复制了 `.sbit`，遗漏配套 `.smsk`，因此回读提示无法打开掩码文件；首次功能结果通过，但不计作配置校验通过。核对原始与快照位流 SHA-256 完全相同、补入同次生成的 `.smsk` 后，第二次完整流程通过。原始失败日志也保留。

位流 SHA-256：`2b32b5d5f2901136525e4ff22fe515a21b401dadfd235b415cdbef5569bee75e`。掩码 SHA-256：`7435f6eaa9f38482559cdc6973e349d7152843eefa4459ca8ca91df5058e8e9a`。

## 证据与复现

- `run1.uart.log` / `.program.log`：首次自检及缺失掩码诊断。
- `run2.uart.log` / `.program.log`：第二次自检、下载和配置校验。
- `manifest.json`：测试对象、输入与位流哈希、串口、每次测试状态。
- `hashes.json`：原始日志与 manifest 的 SHA-256。

执行时使用冻结快照中的旧下载脚本，避免 AG 改名后的输出文件名与旧 profile 冲突：

```powershell
.\build\hw-test-102mhz-20261029-184846\scripts\program_board.ps1 `
  -Profile D:\FPGA\RiscV\build\hw-test-102mhz-20261029-184846\profile `
  -Port COM12 -Expected 'CACHE PASS DDR code warm-hit self-modify maintenance\r?\n' `
  -Output D:\FPGA\RiscV\build\hw-test-102mhz-20261029-184846\run2.uart.log `
  -TimeoutSeconds 120
```

该本机快照包含位流、配套掩码、冻结输入、时序报告和厂商依赖。跨机器复现应从对应冻结版本重建并核对 manifest，不能将当前 AG 脚本直接套用旧 profile。

未写外部 Flash；未做断电冷启动、全容量/长期压力或温压测试。本固件是 DDR/Cache 自检，未测本位流正式 CoreMark 成绩。PLL 频率按标称晶振与配置比例计算，未用外部频率计校准。
