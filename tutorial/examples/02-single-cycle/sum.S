# Assemble as RV32I without compressed instructions. See tutorial for commands.
.option norvc
.text
.globl _start
_start:
    addi x1, x0, 0       # sum = 0
    addi x2, x0, 5       # remaining = 5
loop:
    add  x1, x1, x2
    addi x2, x2, -1
    bne  x2, x0, loop
    sw   x1, 0(x0)
    lw   x3, 0(x0)
    sub  x4, x3, x1
    beq  x4, x0, good
    addi x7, x0, 99      # must be skipped
good:
    jal  x5, done
    addi x7, x0, 98      # must be skipped
done:
    addi x6, x5, 0       # x5 contains address after jal
    .word 0xffffffff     # teaching model: unsupported => halt
