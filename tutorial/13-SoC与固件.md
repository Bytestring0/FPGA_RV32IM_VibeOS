# 13. 把 CPU 接成 SoC：从复位一直走到串口输出

CPU 单独只能发出地址、数据和控制。它不知道内存从哪里来，也不会凭空出现一个 C 语言的 `main()`。要让它运行项目固件，我们必须同时完成两件事：

1. 硬件侧把指令/数据接口接到存储器和外设，并规定地址与握手。
2. 软件侧把程序编译链接为 CPU 能取到的机器码，设置栈、初始化数据，再调用 `main`。

这一章以项目的最小 TCM 系统为主，沿着“上电→取指→初始化→写 UART→退出”的真实路径走一遍。可选 DDR 有自己的时钟、桥和板级工程，不应混进这个最小路径当成自动已验证。

## 13.1 从一张系统图开始

```text
                    ┌──── rv32_core ────┐
        指令请求/响应│                   │数据请求/响应
             ┌──────┤                   ├────────────┐
             │      └───────────────────┘            │
             ▼                                       ▼
      同一块 TCM 的取指端口                  地址译码/数据端口
      一次返回两个指令字                     ├─ TCM RAM
                                            ├─ UART TX
                                            ├─ GPIO
                                            ├─ mtime/mtimecmp/msip
                                            └─ 未映射地址 → 错误
```

`qinglong_soc` 中有一个 16,384×32 位数组，即 65,536 字节 TCM，默认初始化文件 `build/firmware.hex`。它是统一存储器，指令和数据端口访问同一数组；取指一次读取两个相邻字，数据写按字节使能。

关键源码是 [qinglong_soc.sv](../rtl/qinglong_soc.sv)、[uart_tx.sv](../rtl/uart_tx.sv)、[start.S](../sw/start.S) 和 [link.ld](../sw/link.ld)。

## 13.2 最先冻结地址表

本项目默认设备映射：

| 字节地址 | 读 | 写 |
| --- | --- | --- |
| `0x00000000` 起 64 KiB | TCM | TCM，按 byte strobe 更新 |
| `0x10000000` | UART ready 位 | lane 0 写发送字符 |
| `0x10000004` | 未定义为通用输出寄存器 | 完整字写作为仿真退出码 |
| `0x10000008` | GPIO 当前值 | 按 byte strobe 更新 GPIO |
| `0x10000100/104` | `mtime` 低/高字 | 本实现不提供相应写操作 |
| `0x10000108/10c` | `mtimecmp` 低/高字 | 完整字写相应半部 |
| `0x10000110` | 软件中断位 | lane 0 的 bit 0 设置或清除 |
| `0x10000200` | DDR 启用时状态 | 禁止写；未启用 DDR 时访问错误 |

其他外设地址产生错误响应。地址表是青龙平台约定，不是 RISC-V ISA 强制所有处理器都有这些地址。读写准确行为、使能和 DDR 条件以 [SoC 的 MMIO case](../rtl/qinglong_soc.sv#L185-L227) 为准。

软件用 `volatile` 访问设备：

```c
#define MMIO32(addr) (*(volatile unsigned int *)(addr))
MMIO32(0x10000008u) = 1;
```

`volatile` 表示这些访问有外部可见效果，编译器不能像普通局部变量那样消掉或合并；它不自动解决硬件握手、缓存一致性或多核同步问题。

## 13.3 先写同步 TCM，再接握手

项目 TCM 的关键逻辑：

```systemverilog
if (iv && ir && ia < RAM_WORDS * 4)
    ram_i_data <= ram[ia[AW+1:2]];
if (iv && ir && ia < RAM_WORDS * 4 - 4)
    ram_i_second <= ram[ia[AW+1:2] + 1'b1];
if (dv && dr && da < RAM_WORDS * 4) begin
    ram_d_data <= ram[da[AW+1:2]];
    if (dw)
        for (j = 0; j < 4; j = j + 1)
            if (ds[j]) ram[da[AW+1:2]][8*j +: 8] <= dd[8*j +: 8];
end
```

读取在上升沿进行，数据寄存后再作为响应。`iv && ir`、`dv && dr` 是请求接收事件，不是单独看 valid。对指令端，一个请求可返回两个字，两个错误位分别判断首字与第二字是否在 RAM 范围；边界最后一个字可成功而第二字报错。

不要给整个 RAM 数组加同步清零来“让波形好看”：目标 FPGA 的块 RAM 推断可能因此失败。项目用 `$readmemh` 提供初始化，复位控制响应状态而不逐项清空数组。不同 FPGA 工具对初始化、读写同地址行为支持不同，应使用综合报告和目标器件文档核对；项目软件对代码修改后使用 FENCE.I，不依赖同地址并发读写得到某个特定值。

## 13.4 外设响应与 UART 等待

数据请求若不在 TCM/可选 DDR 中，就进入 MMIO 译码。对 UART 的 lane 0 写：

- `SIM_FAST_UART=1`：仿真直接给出 `console_valid/console_data`，快速观察字符。
- 正常模式：保存字符、置 `uart_wait`，当 TX 接受字符才给数据端完成响应；此时 CPU 可以等，而不需要假装 UART 每拍都能发送。

这意味着软件即使不轮询 ready，项目 SoC 的写路径仍会等待 TX 接收；但其他硬件平台可能采用不同协议。快速仿真打印只检查字符流，不证明实际 UART 的电平、波特率或接线正确。

项目还有可选 `PIPELINED_MMIO`：先保存请求的地址、写数据、byte strobe 和读写标志，再下一阶段译码设备，目的是减短地址译码路径；不能只在代码里打开参数而不保持响应与原请求的对应关系。

## 13.5 手写一个 UART TX

UART 8N1 一帧有 10 个位时间：

```text
空闲高电平 → 起始位 0 → 8 个数据位（最低位先）→ 停止位 1 → 空闲高电平
```

比如发送 ASCII `A = 0x41 = 01000001`，从最低位开始的八个数据位依次是 `1,0,0,0,0,0,1,0`。发送端和接收端必须就波特率、数据位、校验和停止位达成一致。

项目 [uart_tx.sv](../rtl/uart_tx.sv) 使用：

```systemverilog
localparam integer DIVISOR = CLOCK_HZ / BAUD;
assign ready = bits_left == 0;
assign tx = ready ? 1'b1 : shift[0];
```

收到 `valid && ready` 时把 `{stop, data, start}` 装入 10 位移位寄存器，`bits_left=10`；每经过 `DIVISOR` 个时钟向右移一位、减一。空闲时 TX 为 1。整数分频会有波特率误差：实际位时间取决于真实时钟和整数 DIVISOR，必须计算并检查接收端容差。参数不应让 DIVISOR 为 0；不能仅通过 `console_valid` 测试代替串行波形测试。

## 13.6 固件为什么从地址 0 开始

核心默认 `RESET_PC=0`，链接脚本 [link.ld](../sw/link.ld) 把 RAM 起始地址设为 0、长度 64 KiB，并让 `.text.start` 排在代码段开头：

```ld
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K }
.text : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
```

固件二进制装入 TCM 后，PC=0 处就是启动代码。若链接地址、镜像装载位置和 RESET_PC 不一致，CPU 取到的就不是预期指令；如果改用 Flash 或 DDR 启动，还必须重新设计启动映射和数据搬运，不能只改一个地址。

链接脚本接着安排 `.rodata/.data/.sdata/.bss`，定义 `__stack_top` 为 TCM 顶部，并断言 bss 末尾与栈顶之间至少预留 8 KiB。这个断言不是动态栈溢出检测；程序运行时仍可能因深递归等耗尽栈空间。

## 13.7 `_start` 为什么不直接调用 `main`

项目 [start.S](../sw/start.S) 的步骤：

```asm
la gp, __global_pointer$
la sp, __stack_top
la t0, default_trap
csrw mtvec, t0
la t0, __bss_start
la t1, __bss_end
# 逐字清零 [__bss_start, __bss_end)
call main
```

`gp` 和 `sp` 是 ABI 使用的全局指针与栈指针；没有初始化就调用 C，编译器生成的栈访问可能写到未知地址。`.bss` 表示未显式初始化的全局/静态数据，应在程序开始时清零；TCM 初始化镜像虽用零补满，启动代码仍明确执行初始化。

设置 `mtvec` 让意外陷阱有最基本的去处。`main` 返回值在 ABI 的 `a0`，启动代码把它写到仿真退出地址，然后死循环；这不是操作系统的 `exit()`。默认 handler 读取 `mcause` 并报告非零退出码，同样是最小调试设施，不是完整的中断服务程序。

## 13.8 ELF 怎么变成 RAM 能读的文件

以 `make smoke` 的规则为例：

```text
start.S + smoke.c + link.ld
  → 交叉编译/链接 build/smoke.elf
  → objcopy -O binary 得到 build/smoke.bin
  → bin2hex.py 得到 build/smoke.hex
  → 复制为 build/firmware.hex
  → $readmemh 初始化 SoC 仿真 RAM
```

[bin2hex.py](../scripts/bin2hex.py) 每 4 个小端字节组成一行 8 位十六进制数，默认补齐到 16,384 行，超出 TCM 容量则报错。例如二进制字节 `93 02 70 00` 表示指令字 `00700293`，hex 行写 `00700293`，不是把文本顺序误写成 `93027000`。

ELF 的地址布局由链接脚本决定；简单的原始 binary 转换不携带完整符号信息。看结果用 `objdump -d` 对照地址和机器码，出现缺字节或地址错位时先检查链接与装载，不要先怀疑 ALU。

## 13.9 跟踪 smoke：它到底验证了什么

[smoke.c](../sw/smoke.c) 初始化 16 个数据，求和应为 4616；再计算一个 hash，用 `puts_uart` 和 `puthex` 输出启动信息，返回 0。`platform.h` 中 `MMIO32` 使用 volatile，`putchar_uart` 向 UART 地址写低字节。这样同一个程序同时走过：

```text
取指 → 算术/循环 → TCM 数据读写 → 条件分支 → UART MMIO → 退出 MMIO
```

它是重要的端到端冒烟测试，却并不覆盖全部 ISA、异常、双发射冲突或物理串口波形。看到 `boot OK` 不代表所有功能正确。

`read_cycles()` 采用 `rdcycleh → rdcycle → rdcycleh`；如果两次高半不同，说明低半可能在读取时翻转，重新读取避免拼出不一致的 64 位数。修改 64 位 `mtimecmp` 的两个半字时也要考虑中间值可能暂时触发中断，软件应按平台和规范采用安全顺序。

## 13.10 自己复现时要保护已有结果

`make smoke` 会写 `build/firmware.hex` 等共享文件。先建立独立项目副本，在 WSL 中依照 [第 14 章](14-验证与调试.md) 执行；不要和 `make coremark` 等同时运行同一个工作目录。开发时分别检查：

1. reset 后 PC 是否为 0，首条指令与反汇编一致；
2. `sp` 是否位于合法 RAM 顶端，bss 循环写地址是否在范围内；
3. 每个 MMIO 请求的地址、写使能、响应是否正确；
4. 仿真输出与退出码是否符合预期；
5. 使用真实串行 TX 波形单独验证 8N1 和时钟分频。

下一章建立系统验证与错误定位流程：[第 14 章](14-验证与调试.md)。可选 DDR 接口的实现和跨时钟细节另见 [第 18 章](18-外部内存与跨时钟.md)。
