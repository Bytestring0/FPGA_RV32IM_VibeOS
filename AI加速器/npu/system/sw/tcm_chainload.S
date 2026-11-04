/* Enter from DDR and replace TCM only after the complete image is available. */
.section .text.chainload,"ax",@progbits
.balign 4
.globl tcm_chainload
tcm_chainload:
    csrw mie, zero
    csrci mstatus, 8
    la t0, tcm_image_start
    la t2, tcm_image_end
    li t1, 0
1:  bgeu t0, t2, 2f
    lw t3, 0(t0)
    sw t3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j 1b
2:  fence iorw, iorw
    fence.i
    li t0, 0
    jr t0
