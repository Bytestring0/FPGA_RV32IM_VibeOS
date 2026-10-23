.section .text.start
.globl _start
.macro EXPECT cause, value, resume, fault
    li s0, \cause
    li s1, \value
    la s2, \resume
    la s4, \fault
.endm
_start:
    la t0, handler
    csrw mtvec, t0
    li s3, 0
    li s5, 0
    EXPECT 11, 0, after_ecall, fault_ecall
fault_ecall: ecall
    addi s5, s5, 1
after_ecall:
    EXPECT 2, 0xffffffff, after_illegal, fault_illegal
fault_illegal: .word 0xffffffff
    addi s5, s5, 1
after_illegal:
    EXPECT 3, 0, after_break, fault_break
    la s1, fault_break
fault_break: ebreak
after_break:
    EXPECT 4, 1, after_lw, fault_lw
fault_lw: lw a0, 1(zero)
after_lw:
    EXPECT 6, 2, after_sw, fault_sw
fault_sw: sw a0, 2(zero)
after_sw:
    EXPECT 4, 3, after_lh, fault_lh
fault_lh: lh a0, 3(zero)
after_lh:
    EXPECT 6, 1, after_sh, fault_sh
fault_sh: sh a0, 1(zero)
after_sh:
    EXPECT 5, 0x20000000, after_access_load, fault_access_load
    lui t1, 0x20000
fault_access_load: lw a0, 0(t1)
after_access_load:
    EXPECT 7, 0x20000000, after_access_store, fault_access_store
    lui t1, 0x20000
fault_access_store: sw a0, 0(t1)
after_access_store:
    EXPECT 0, 2, after_jalr, fault_jalr
    li t1, 2
fault_jalr: jalr a0, t1, 0
after_jalr:
    EXPECT 2, 0xc0001073, after_ro, fault_ro
fault_ro: csrw cycle, zero
after_ro:
    EXPECT 2, 0x77702573, after_csr, fault_csr
fault_csr: csrr a0, 0x777
after_csr:
    EXPECT 1, 0x20000000, after_ifault, fault_ifault
    li s4, 0x20000000
    lui t1, 0x20000
fault_ifault: jalr zero, t1, 0
after_ifault:
    // CSRRC/CSRRS and immediate variants, fixed MPP WARL, scratch RMW.
    li t0, 0x12345678
    csrw mscratch, t0
    csrr t1, mscratch
    bne t0, t1, fail
    csrrwi t1, mscratch, 31
    bne t0, t1, fail
    csrrci t1, mscratch, 3
    li t2, 31
    bne t1, t2, fail
    csrrsi t1, mscratch, 1
    li t2, 28
    bne t1, t2, fail
    csrr t1, mscratch
    li t2, 29
    bne t1, t2, fail
    // Software interrupt. Handler clears level before MRET.
    EXPECT 0x80000003, 0, after_softirq, soft_resume
    li t0, 8
    csrw mie, t0
    lui t1, 0x10000
    li t2, 1
    sw t2, 0x110(t1)
    csrs mstatus, t0
soft_resume: nop
after_softirq:
    csrci mstatus, 8
    // Timer interrupt, pending before global enable.
    EXPECT 0x80000007, 0, after_timer, timer_resume
    li t0, 128
    csrw mie, t0
    lui t1, 0x10000
    sw zero, 0x108(t1)
    sw zero, 0x10c(t1)
    li t0, 8
    csrs mstatus, t0
timer_resume: nop
after_timer:
    csrw mie, zero
    bnez s5, fail
    li t0, 15
    bne s3, t0, fail
    lui t0, 0x10000
    sw zero, 4(t0)
1:  j 1b
.balign 4
handler:
    csrr t0, mcause
    bne t0, s0, fail
    csrr t0, mtval
    bne t0, s1, fail
    csrr t0, mepc
    bne t0, s4, fail
    csrr t0, mstatus
    andi t0, t0, 8
    bnez t0, fail
    addi s3, s3, 1
    lui t0, 0x10000
    sw zero, 0x110(t0)
    li t1, -1
    sw t1, 0x10c(t0)
    sw t1, 0x108(t0)
    // Disable next interrupt until the next test explicitly enables it.
    csrrci zero, mstatus, 8
    csrw mie, zero
    csrw mepc, s2
    mret
fail:
    lui t0, 0x10000
    addi t1, s3, 1
    sw t1, 4(t0)
2:  j 2b
