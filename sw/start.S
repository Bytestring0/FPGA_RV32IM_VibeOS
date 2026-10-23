.section .text.start
.globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __stack_top
    la t0, default_trap
    csrw mtvec, t0
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call main
    lui t0, 0x10000
    sw a0, 4(t0)
3:  j 3b
.balign 4
default_trap:
    csrr a0, mcause
    ori a0, a0, 128
    lui t0, 0x10000
    sw a0, 4(t0)
    j 3b
