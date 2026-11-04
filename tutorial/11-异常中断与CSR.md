# 11. 写异常、中断和 CSR：让 CPU 在意外发生后有路可走

在教学 CPU 里，遇到不认识的指令就把 `halted` 置 1。但真正的软件需要知道**为什么出错、发生在哪里、接下来去哪里处理**。例如访问未对齐地址时，处理程序可能记录错误；定时器中断时，处理程序可能更新计数并返回原程序。

青龙实现的是机器态（M-mode）CSR 与陷阱机制，不等于实现了 S/U 态、页表、虚拟内存或完整操作系统。赛题说“完整异常/中断”，具体特权范围应和主办方核对；不要用“M-mode 测试通过”直接宣布所有 RISC-V 特权功能均已实现。

先打开 [rv32_csr.sv](../rtl/rv32_csr.sv) 与核心 [异常判断](../rtl/rv32_core.sv#L292-L324)、[陷阱控制](../rtl/rv32_core.sv#L439-L454)。这一章分别写寄存器本身、CPU 发出的陷阱事件、软件处理与验证。

## 11.1 异常和中断有什么不同

- **同步异常**由某条具体指令触发：非法指令、取指错误、Load 不对齐、ECALL 等。`mepc` 要指向发生异常的指令。
- **中断**由外部事件请求，例如定时器。它不是某条普通指令本身算错了；CPU 要在清晰的指令边界接收它，并在处理结束后继续未执行的程序。

两者都使用 trap 入口，但 `mcause` 最高位标记是否中断，cause 数字的解释也不同。将两者都简单描述为“跳到处理函数”会漏掉精确状态和返回位置。

## 11.2 先实现需要保存的状态

项目 `rv32_csr` 中最重要的寄存器：

| CSR | 地址 | 我们用它做什么 |
| --- | --- | --- |
| `mstatus` | 0x300 | 机器态中断总使能 MIE、先前使能 MPIE 等 |
| `misa` | 0x301 | 固定的 ISA 标识，本项目返回 RV32 I+M 标记 |
| `mie` | 0x304 | 分别允许软件、定时器、外部中断 |
| `mtvec` | 0x305 | 陷阱入口基址及直接/向量模式 |
| `mscratch` | 0x340 | 供机器态软件使用的暂存 |
| `mepc` | 0x341 | 陷阱返回 PC |
| `mcause` | 0x342 | 中断标志与原因 |
| `mtval` | 0x343 | 附加信息，例如错误地址或非法指令 |
| `mip` | 0x344 | 当前中断 pending 输入的映射 |
| `mcycle/minstret` 及高半 | 0xb00/0xb02/0xb80/0xb82 | 64 位周期和退休计数 |
| `cycle/instret` 及高半 | 0xc00/0xc02/0xc80/0xc82 | 对应只读计数视图 |

还有 `mcountinhibit`、固定身份值等，完整映射见 [CSR 读取 case](../rtl/rv32_csr.sv#L34-L57)。CSR 具有 WARL 等约束：不是所有写入位都必须原样保存，项目对 `mstatus/mie/mtvec/mepc` 等按代码进行掩码或对齐。`misa` 标记不列出独立的 Z 扩展位，不能仅靠这个值判断所有可用扩展。

## 11.3 CSR 指令：读取旧值，必要时写入新值

以 `csrrw x5,mscratch,x6` 为例：

```text
旧值 = mscratch
新值 = x6
成功时 x5 ← 旧值，mscratch ← 新值
```

`CSRRS` 用旧值 OR 源操作数；`CSRRC` 用旧值 AND NOT 源操作数。立即数形式把指令中的 5 位 zimm 当常数，不读相应编号的寄存器。项目 EX 中的计算是：

```systemverilog
csr_source = x_dec.csr_imm ? {27'b0, x_dec.rs1} : a;
csr_we = x_dec.csr && (x_dec.funct3[1:0] == 1 || x_dec.rs1 != 0);
case (x_dec.funct3[1:0])
    1: csr_new = csr_source;
    2: csr_new = csr_read_data | csr_source;
    default: csr_new = csr_read_data & ~csr_source;
endcase
```

对于 CSRRS/CSRRC，源字段为零时不写 CSR；因此读取只读 CSR 的零源形式可合法，而真正企图写只读 CSR 要陷入非法指令。未知 CSR 也应被拒绝。结果与写入必须受到 `csr_legal/csr_readonly`、异常、有效位和提交条件约束；不应在 ID 刚看到 CSR 指令时立刻修改 CSR。

项目让 CSR 指令串行执行，避免前后指令与 CSR 可见状态重叠造成复杂依赖。CSR 写使能在 MEM 完成且没有 trap 时产生，见 [实例连接](../rtl/rv32_core.sv#L230-L241)。

## 11.4 同步异常：记录故障指令及原因

EX 处理本地异常并为 MEM 保存 `x_cause/x_tval`。项目代码的判断顺序：

| 条件 | cause | tval |
| --- | ---: | --- |
| 该取指字返回错误 | 1，指令访问错误 | 故障 PC |
| 非法编码、未知/非法写 CSR | 2，非法指令 | 指令字 |
| EBREAK | 3，断点 | PC |
| ECALL（M-mode） | 11 | 0 |
| 实际跳转目标不对齐 | 0，指令地址不对齐 | 跳转目标 |
| Load 不对齐 | 4 | 有效地址 |
| Store 不对齐 | 6 | 有效地址 |

外部数据总线响应错误在 MEM 才知道，Load 为 cause 5，Store 为 cause 7，tval 为原始地址。

代码中的 `if ... else if ...` 表示同一条指令多个条件同时成立时的优先顺序；不能在另一个 always 块又随意覆盖结果。由于项目不支持压缩指令，正常指令地址要求 4 字节对齐。未采取的条件分支不会因其未使用的目标而报地址不对齐。

## 11.5 trap 输入到 CSR 时发生什么

项目 [CSR 时序块](../rtl/rv32_csr.sv#L85-L92)：

```systemverilog
if (trap) begin
    mepc <= {trap_pc[31:2], 2'b0};
    mcause <= {trap_interrupt, trap_cause[30:0]};
    mtval <= trap_value;
    mstatus[7] <= mstatus[3];
    mstatus[3] <= 0;
end else if (mret) begin
    mstatus[3] <= mstatus[7];
    mstatus[7] <= 1;
end
```

用普通语言解释：进入 trap 时保存 PC、原因和附加信息，把原来的全局中断使能 MIE 保存到 MPIE，再关掉 MIE；MRET 恢复 MIE。项目只实现 M-mode 并固定相应 MPP 值，不能把这几行理解为完整多特权级切换实现。

普通 CSR 写和 trap 在同一时钟块中存在优先关系；例如 trap 对 `mepc/mcause/mtval` 的写位于普通 CSR 写之后。系统设计还利用 serial 和流水排空避免不应出现的重叠。更改时必须明确同时事件的优先级并加入测试。

## 11.6 `mtvec`：同步异常和中断入口不一定一样

项目把 `mtvec[31:2]` 当基地址：

```text
直接模式：所有 trap → base
向量模式：同步异常 → base；中断 → base + 4 * irq_cause
```

例如 `mtvec=0x101`，表示 base=`0x100`、向量模式；机器外部中断 cause=11，所以入口是 `0x100 + 4*11 = 0x12c`。不要把 `mcause` 的最高中断标志整字乘以 4；项目使用不带该标志的 `irq_cause`。

软件写入时项目只允许 mode 0 或 1 的实际表达；看源码中的 WARL 规则，而不是假设所有低两位组合有效。

## 11.7 中断怎样从外部走到核心

项目的 `mip` 把三个同步输入映射到 bit 3、7、11：

```systemverilog
mip[3] = irq_software;
mip[7] = irq_timer;
mip[11] = irq_external;
enabled = mip & mie;
irq_pending = mstatus[3] && |enabled;
irq_cause = enabled[11] ? 11 : enabled[3] ? 3 : 7;
```

也就是说，某个源 pending 不等于必定进入 handler：对应 `mie` 位和全局 `mstatus.MIE` 都要开启。本实现同时 pending 时优先外部、再软件、再定时器。外部中断输入必须同步到 CPU 时钟；异步引脚应在更外层做同步/事件保持，不能直接假设输入满足建立保持时间。

SoC 的定时器比较 `mtime >= mtimecmp`，软件中断来自 `msip`。处理程序要清除/推进中断源；仅执行 MRET 而 pending 条件仍为真，会再次进入中断。这不是 CPU“无故重复中断”。

## 11.8 在哪个指令边界接收中断

项目选择一个保守边界：

```systemverilog
irq_take = irq_pending && id_valid && !x_valid && !m_valid && !w_valid;
```

有 pending 时阻止新的普通发射，并等待 EX/MEM/WB 排空；当队首存在时，以其 `id_pc` 作为中断返回 PC。这样更老的已发射指令已退休，队首尚未执行，handler 返回后可以从该 PC 继续。

它比让中断在任意流水级立即打断更容易实现精确状态，但有中断响应延迟。如果前端没有有效指令、访存一直不响应或某个单元无法结束，何时能接收中断也取决于整体系统；不要承诺一个代码里没有保证的固定最大延迟。

项目的 trap 输出写成 `trap_valid = mem_trap || irq_take`，PC/cause/value 的选择表达式优先 MEM 异常。不过当前 `irq_take` 要求 `!m_valid`，而 `mem_trap` 依赖有效的 MEM 指令，因此正常控制下二者不能同拍为真；不要把选择表达式误解为一个实际会发生的同拍仲裁场景。若改动接收边界，应重新分析较老同步异常与异步事件的优先级并测试。

## 11.9 一个完整的软件陷阱路径

这是用于理解的汇编流程，具体入口/寄存器保存按你自己的 ABI 和测试环境编写：

```text
启动时：设置 mtvec → handler
发生异常：硬件保存 mepc/mcause/mtval，重定向到 handler
handler：先保存会破坏的通用寄存器；读取 mcause 和 mtval
同步异常：决定是停止、修复后重试，还是由软件调整 mepc 跳过
中断：清除/推进设备中断源
恢复保存的寄存器，执行 mret → mepc
```

不能把所有异常一律把 `mepc` 加 4：访问错误可能需要修复后重试；未来若支持不同长度指令，跳过长度也不能硬编码。当前项目无 C 扩展，自己的定向测试可以针对已知 4 字节异常明确加 4。

`mret` 是串行操作，项目在 EX 计算 `return_pc` 并重定向，成功完成后由 CSR 恢复状态。`FENCE.I` 则处理指令缓存/前端可见性，在当前实现中刷新前端和预测相关状态；普通 FENCE 也是串行屏障。

## 11.10 先测 CSR，再测整个 CPU

在项目根目录运行直接针对真实模块的测试：

```powershell
iverilog -g2012 -s project_csr_tb -o "$env:TEMP\project_csr.vvp" `
  rtl/rv32_csr.sv tutorial/examples/06-project-csr/project_csr_tb.sv
if ($LASTEXITCODE -ne 0) { throw '编译失败' }
vvp "$env:TEMP\project_csr.vvp"
if ($LASTEXITCODE -ne 0) { throw '测试失败' }
```

[测试台](examples/06-project-csr/project_csr_tb.sv) 设置向量模式、中断使能，模拟外部与定时中断同时 pending，检查优先级、`0x12c` 入口、`mepc/mcause/mstatus` 和 MRET 恢复；预期 `PASS project_csr: trap, vector and return`。它直接驱动 trap，不证明核心正确判断了具体指令或精确提交。

之后在完整核心/SoC 上还需要至少这些用例：

1. 非法指令：检查 cause=2、mepc 是指令 PC、mtval 是指令字；确认不写 rd。
2. `LW` 不对齐：检查 cause=4、mtval 是原始地址，且没有总线请求。
3. Load/Store 访问错误：分别检查 cause=5/7 与对应 PC/地址。
4. ECALL、EBREAK、MRET：handler 正确保存/恢复寄存器与返回位置。
5. 分别关闭全局 MIE 和单源 mie：pending 时不进入；开启后进入并清源。
6. 中断等待老指令排空：检查没有丢失或重复退休，也不提前执行 handler。
7. 双发射异常：年轻 Load 出错保留较老 ALU；老 Load 出错丢弃较年轻 ALU。
8. CSRRS 零源读只读 CSR 应与实际写只读 CSR 区分；未知 CSR 触发非法指令。

项目已有 [trap_tests.S](../tests/trap_tests.S) 和 [interrupt_tests.S](../tests/interrupt_tests.S)；第 14 章解释怎样在隔离副本运行。接下来补预测器与乘除单元：[第 12 章](12-预测和乘除.md)。
