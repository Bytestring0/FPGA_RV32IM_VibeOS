# RV 侧接口文档

适用源码：`rtl/rv32_core.sv`、`qinglong_soc.sv`、`qinglong_extmem.sv`、`qinglong_l1cache.sv`、`qinglong_hmic_bridge.sv`。2026-10-29。本文区分已经连通的硬件接口与后续 AI 接口规划。

## 1. CPU 与软件约定

RV32、小端、ILP32 ABI，复位 PC 为 0；IF1/IF2/ID/EX/MEM/WB 六级，顺序双发射、顺序提交。支持 RV32I/M/Zicsr/Zifencei/Zba/Zbb，不支持 C/A、MMU、S/U 特权级或 Linux。推荐裸机编译参数 `-march=rv32im_zicsr_zifencei -mabi=ilp32`；使用扩展时追加 `_zba_zbb`。工具名可为提供 RV32 multilib 的 riscv64-unknown-elf-gcc，不代表生成 RV64 程序。

`clk` 为 CPU 时钟，`rst` 高有效同步复位。通用核默认 MULH_PIPELINE=0；DDR 板级候选使用 1。MULH/MULHSU/MULHU 结果在 WB 可供消费者使用。MIN/MAX/MINU/MAXU 在已有 MEM 级计算，相邻消费者等待 WB。硬件没有内存保护，后续脚本运行时必须进行软件地址和指令限制。

## 2. CPU 请求/响应接口

所有握手在 `clk` 上升沿以 valid && ready 接受。每个端口最多一笔未完成事务，响应至少晚一拍，响应 valid 和 payload 在 ready 前保持。复位清空事务，外部从设备必须同步复位。

| 通道 | 请求 | 响应 |
|---|---|---|
| 取指 | i_req_valid/ready、32 位字节地址 i_req_addr | i_rsp_valid/ready、64 位 i_rsp_data、2 位 i_rsp_error |
| 数据 | d_req_valid/ready、32 位 d_req_addr/wdata、4 位 wstrb、write | d_rsp_valid/ready、32 位 d_rsp_data、error |

取指返回低 32 位为 addr 指令，高 32 位为 addr+4；error 位 0/1 分别对应这两个字。指令必须 4 字节对齐。取指在尚未握手时允许因重定向取消，因此不能直接接要求 VALID 保持的标准 AXI 主口；需桥接和事务缓存。已接受的错误路径取指仍须完成响应，由 CPU 丢弃。数据地址是字节地址，写数据按地址低位移到相应字节 lane，wstrb[k] 控制 wdata[8*k+:8]。写事务也必须返回一次响应。访问错误转为精确异常；不支持未对齐访存拆包。

`fence_i_o` 是 CPU 维护请求；SoC 锁存请求、排空已接受事务、写回脏 D 行并失效 I/D Cache 与取指行缓冲。外部维护不能只清 I Cache 而丢弃脏数据。

## 3. 地址空间与 MMIO

默认 TCM 64 KiB；TCM 和 MMIO 不缓存。除 UART 字节输出等明确支持的掩码外，建议使用对齐的 32 位访问。未知地址返回访问错误。

| 字节地址 | 功能与访问方式 |
|---|---|
| 0x00000000–0x0000ffff | TCM，复位代码、栈和静态数据；容量可参数化 |
| 0x80000000–0xbfffffff | 1 GiB 板载 DDR3，只有 DDR_ENABLE 时有效 |
| 0x10000000 | UART TX，写低字节；发送忙时硬件背压；115200、8N1。没有 UART RX |
| 0x10000004 | 全字写退出码，并置 done；调试用途 |
| 0x10000008 | 32 位 GPIO 输出寄存器，可按字节写；板级 LED 使用低位 |
| 0x10000100 / 104 | mtime 低/高 32 位，只读，每 CPU 周期加一 |
| 0x10000108 / 10c | mtimecmp 低/高 32 位，全字写；mtime >= mtimecmp 产生定时中断 |
| 0x10000110 | 软件中断 msip，bit0 可读写 |
| 0x10000200 | DDR/Cache 状态，只读：bit0 ready、1 DDR fault、2 DDR 存在、3维护忙、4 Cache fault、5 Cache 存在 |
| 0x10000204 | bit0 写 1 请求全 Cache 维护；读 bit0 busy、bit1 error |
| 0x10000210 / 214 | I 行命中 / 未命中计数 |
| 0x10000218 / 21c | D 行命中 / 未命中计数 |
| 0x10000220 / 224 | D 脏行写回 / 取指行缓冲命中计数 |

计数器 32 位自然回绕，复位清零。DDR 未校准不能访问该窗口；先检查 ready 与 fault。Cache 复位逐组初始化，busy 清零后可接受请求。维护错误后 Cache 保持故障，不能继续作为成功操作使用；软件应报错并受控复位。

读取 mtime 使用 high/low/high 重读直到两个 high 相等。更新 mtimecmp 时先写 low=0xffffffff，再写 high、最后写 low，避免中间值产生提前中断。同步硬件访问使用 volatile 与 `fence rw,rw`，不能依赖编译器普通内存访问顺序。

## 4. 异常、中断与调试

M 态 CSR：mstatus、misa、mie、mtvec、mscratch、mepc、mcause、mtval、mip、mcountinhibit、mcycle/minstret 及高半字。misa 报告 I+M，附加扩展能力按平台约定；未知 CSR 访问产生非法指令异常。mtvec 支持 direct 和 vectored；MRET 恢复中断状态。外部/软件/定时中断源分别为 irq_external、msip、mtime 比较；外部中断需电平同步，在板级中当前没有接入 AI 中断源。无 PLIC、S 模式或 PMP。

核导出两个有序提交通道 retire_valid/pc/insn/rd/data 与 retire_second_*，以及 trap_valid/pc/cause/value、cycle_count、instret_count、dual_retire_count、branch_count、mispredict_count、load_stall_count。ILA 建议同 CPU 时钟采样上述提交、trap、取指/数据握手及 Cache busy/error，触发条件选 trap 或约定 CoreMark 开始 PC；必须在最终带调试核的工程中重新检查资源与时序。目前这些是观察信号和采集方案，尚无新工程的实板 ILA 波形。

## 5. Cache 与 DDR 行接口

板级 I/D 各 16 KiB、2 路、32 字节行、256 组；通用模块默认各 8 KiB，可参数化。D Cache 为 write-back/write-allocate，失效路优先、LRU 替换。阻塞式结构，共享单笔下游事务，非 MSHR 多未决缓存。

SoC 外部接口在 CPU 域：req_valid/ready、30 位 req_addr、write、256 位 wdata、32 位 wstrb；rsp_valid/ready、256 位 data、error。req_addr 是 DDR 窗口内的字节偏移，要求 32 字节对齐；bit[7:0] 是最低地址字节。一次请求对应一个 32 字节行，响应保持到握手。写响应表示控制器接受写入，不表示断电持久化。

桥接到 PDS HMIC_S 1.10，CPU 102 MHz、DDR UI 125 MHz、DDR3 1000 MT/s、32 位物理数据、2 颗 x16 器件共 1 GiB。HMIC 是简化 AXI：没有 WVALID、B 通道、RREADY，不能与标准 AXI 直接互换。AXI 地址为窗口偏移除以 4，256 位单拍 LEN=0 对应 DDR3 BL8。读数据到达前必须预留响应存储，校验 RID/RLAST；写数据在 AW 至 WREADY 期间保持，校验 LAST/ID。握手超时和协议错误进入 fault。

CPU/UI 跨域采用稳定 payload 的 toggle mailbox，控制信号两级同步、复位各域同步释放，payload 有显式飞行时间约束。两域必须共用异步复位来源；不能删除 `fpga/board/ddr_cdc.fdc` 或把所有跨域路径一律设为 false path。

## 6. 后续 AI/DMA 接入边界

当前没有接入队伍的 AI 加速器 CSR、DMA 仲裁器或完成中断；不能使用尚未实现的 MMIO 地址。可在 CPU 域行接口之后添加仲裁，所有请求/响应必须携带并保存 owner，复位排空，不能直接并接两个 master。模型权重、KV cache、HTML 文本可分配在 DDR 窗口内，具体内存分区见浏览器方案文档，尚不构成硬件保护。

DMA 读 CPU 数据前：软件停止修改缓冲区、执行 fence、请求全 Cache 维护并轮询 busy/error，再启动 DMA。DMA 写入前同样维护；完成后再次维护/失效，然后 CPU 才可读取。DMA 期间禁止 CPU 修改同一缓存行。CPU 生成可执行代码后必须 FENCE.I；普通 FENCE 不代替非一致性 Cache 维护。HTML 应作为受限文本解释，不应将模型输出直接当任意机器码跳转执行。

## 7. 复现入口

`make check` 为 CPU 回归，`make cache-test` 为 Cache/DDR SoC 检查。`scripts/prepare_ddr_board.ps1` 生成包含全部源文件的 PDS 工程；只有最终时序通过才允许正常生成位流。`-CpuLogicRegion xmin,xmax,ymin,ymax` 使用 floorplan 坐标，`-PlacementSeed` 指定布局种子。实际通过的版本必须同时保存源码、固件、约束、报告和哈希，不能用另一个 profile 的报告证明当前 RTL。
