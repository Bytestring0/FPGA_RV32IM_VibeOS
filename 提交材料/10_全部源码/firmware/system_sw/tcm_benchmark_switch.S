/* DDR-resident switch: preserve the complete TCM image and caller ABI. */
.section .text.tcm_benchmark_switch,"ax",@progbits
.balign 4
.globl tcm_benchmark_switch
tcm_benchmark_switch:
 li t0,0xbf0d0000
 la t1,benchmark_return
 sw t1,0(t0)
 sw ra,4(t0)
 sw sp,8(t0)
 sw gp,12(t0)
 sw tp,16(t0)
 sw s0,20(t0)
 sw s1,24(t0)
 sw s2,28(t0)
 sw s3,32(t0)
 sw s4,36(t0)
 sw s5,40(t0)
 sw s6,44(t0)
 sw s7,48(t0)
 sw s8,52(t0)
 sw s9,56(t0)
 sw s10,60(t0)
 sw s11,64(t0)
 csrr t1,mtvec
 sw t1,68(t0)
 csrr t1,mie
 sw t1,72(t0)
 csrr t1,mstatus
 sw t1,76(t0)
 csrw mie,zero
 csrci mstatus,8
 li t0,0
 li t1,0xbf0c0000
 li t2,65536
1:
 lw t3,0(t0)
 sw t3,0(t1)
 addi t0,t0,4
 addi t1,t1,4
 addi t2,t2,-4
 bnez t2,1b
 la t0,tcm_benchmark_image_start
 la t1,tcm_benchmark_image_end
 li t2,0
2:
 bgeu t0,t1,3f
 lw t3,0(t0)
 sw t3,0(t2)
 addi t0,t0,4
 addi t2,t2,4
 j 2b
3:
 fence iorw,iorw
 fence.i
 li t0,0
 jr t0
benchmark_return:
 li t0,0xbf0c0000
 li t1,0
 li t2,65536
4:
 lw t3,0(t0)
 sw t3,0(t1)
 addi t0,t0,4
 addi t1,t1,4
 addi t2,t2,-4
 bnez t2,4b
 fence iorw,iorw
 fence.i
 li t0,0xbf0d0000
 lw ra,4(t0)
 lw sp,8(t0)
 lw gp,12(t0)
 lw tp,16(t0)
 lw s0,20(t0)
 lw s1,24(t0)
 lw s2,28(t0)
 lw s3,32(t0)
 lw s4,36(t0)
 lw s5,40(t0)
 lw s6,44(t0)
 lw s7,48(t0)
 lw s8,52(t0)
 lw s9,56(t0)
 lw s10,60(t0)
 lw s11,64(t0)
 lw t1,68(t0)
 csrw mtvec,t1
 lw t1,72(t0)
 csrw mie,t1
 lw t1,76(t0)
 csrw mstatus,t1
 ret
