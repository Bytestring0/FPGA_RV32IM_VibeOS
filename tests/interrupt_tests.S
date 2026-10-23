.section .text.start
.globl _start
_start:
    // External interrupt is held high by the simulator for this test.
    la t0, vectors
    ori t0, t0, 1
    csrw mtvec, t0
    li s0, 0
    li s1, 0
    lui t0, 0x10000
    li t1, 1
    sw t1, 0x110(t0)  // MSIP
    sw zero, 0x108(t0)
    sw zero, 0x10c(t0) // MTIP
    li t1, 0x888
    csrw mie, t1
    csrsi mstatus, 8
    // Three interrupts must be serviced in MEI, MSI, MTI order.
    nop
    li t0, 3
    bne s0, t0, fail
    li t0, 0xb37
    bne s1, t0, fail
    csrr t0, mstatus
    andi t0, t0, 8
    beqz t0, fail
    csrci mstatus, 8
    // Freeze/write/read both halves, then verify cycle carry at RV32 wrap.
    li t0, 5
    csrw mcountinhibit, t0
    li t0, 0x123
    csrw mcycleh, t0
    li t0, -8
    csrw mcycle, t0
    csrr t1, mcycle
    bne t0, t1, fail
    li t0, 0x456
    csrw minstreth, t0
    li t0, 0xabc
    csrw minstret, t0
    csrr t1, minstret
    bne t0, t1, fail
    csrr t1, minstreth
    li t0, 0x456
    bne t0, t1, fail
    csrci mcountinhibit, 1
    .rept 16
    nop
    .endr
    csrr t1, mcycleh
    li t0, 0x124
    bne t0, t1, fail
    lui t0, 0x10000
    sw zero, 4(t0)
1:  j 1b
fail:
    lui t0, 0x10000
    li t1, 99
    sw t1, 4(t0)
2:  j 2b
.balign 256
vectors:
    j fail
    j fail
    j fail
    j software
    j fail
    j fail
    j fail
    j timer
    j fail
    j fail
    j fail
    j external
external:
    li t2, 11
    li t0, 0x800
    csrc mie, t0
    j record
software:
    li t2, 3
    lui t0, 0x10000
    sw zero, 0x110(t0)
    j record
timer:
    li t2, 7
    lui t0, 0x10000
    li t1, -1
    sw t1, 0x10c(t0)
record:
    csrr t0, mcause
    li t1, 0x80000000
    or t1, t1, t2
    bne t0, t1, fail
    csrr t0, mtval
    bnez t0, fail
    csrr t0, mstatus
    andi t0, t0, 8
    bnez t0, fail
    slli s1, s1, 4
    or s1, s1, t2
    addi s0, s0, 1
    mret
