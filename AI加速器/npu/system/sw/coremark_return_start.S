.section .text.start,"ax",@progbits
.globl _start
_start:
 .option push
 .option norelax
 la gp,__global_pointer$
 .option pop
 la sp,__stack_top
 la t0,benchmark_trap
 csrw mtvec,t0
 la t0,__bss_start
 la t1,__bss_end
1: bgeu t0,t1,2f
 sw zero,0(t0)
 addi t0,t0,4
 j 1b
2: call main
 li t0,0xbf0d0100
 sw a0,80(t0)
 j benchmark_exit
.balign 4
benchmark_trap:
 li t0,0xbf0d0100
 csrr t1,mcause
 sw t1,84(t0)
 csrr t1,mepc
 sw t1,88(t0)
 csrr t1,mtval
 sw t1,92(t0)
benchmark_exit:
 fence iorw,iorw
 li t0,0xbf0d0000
 lw t0,0(t0)
 jr t0
