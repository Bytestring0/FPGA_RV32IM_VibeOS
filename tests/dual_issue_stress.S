// Directed ordered-dual-issue regression. Also valid on the scalar baseline.
// gp is the failing test number. MMIO EXIT=0 means all checks passed.
// No stack is used, leaving the final TCM word available for fetch-boundary tests.
.option norvc
.option norelax
.section .text.start
.globl _start

.macro TEST number
    li gp, \number
.endm
.macro EXPECT_TRAP cause, value, fault, resume
    li s8, \cause
    li s9, \value
    la s10, \fault
    la s11, \resume
.endm

_start:
    la t0, trap_handler
    csrw mtvec, t0
    li s3, 0
    li s4, 0xe000

    // RAW within a possible pair: the second instruction needs the new t0.
    TEST 1
    li t0, 99
    .balign 8
    addi t0, zero, 17
    addi t1, t0, 6
    add t2, t0, t1
    li t3, 40
    bne t2, t3, fail

    // WAW must preserve program order even if same-address dual writes exist.
    TEST 2
    .balign 8
    addi t0, zero, 7
    addi t0, zero, 11
    addi t1, t0, 13
    li t2, 24
    bne t1, t2, fail

    // WAR is safe to pair: lane0 observes the old source before lane1 writes.
    TEST 3
    li t1, 33
    .balign 8
    addi t0, t1, 2
    addi t1, zero, 9
    add t2, t0, t1
    li t3, 44
    bne t2, t3, fail

    // Two producer lanes, then two consumers, exercising both bypass sources.
    TEST 4
    .balign 8
    addi t0, zero, 17
    addi t1, zero, 23
    add t2, t0, t1
    xor t3, t0, t1
    li t4, 40
    bne t2, t4, fail
    li t4, 6
    bne t3, t4, fail

    // x0 must not create artificial RAW/WAW hazards or ever change value.
    TEST 5
    .balign 8
    addi zero, zero, 17
    addi t0, zero, 23
    add zero, t0, t0
    addi t1, zero, 9
    li t2, 23
    bne t0, t2, fail
    li t2, 9
    bne t1, t2, fail
    bnez zero, fail

    // Load + independent ALU: wait-state holding must preserve both results.
    TEST 6
    li t0, 0x1234
    sw t0, 0(s4)
    .balign 8
    lw t1, 0(s4)
    addi t2, zero, 7
    add t3, t1, t2
    li t4, 0x123b
    bne t3, t4, fail
    // Store + independent ALU, then consumer and read-after-write.
    .balign 8
    sw t3, 4(s4)
    addi t4, zero, 19
    lw t5, 4(s4)
    add t5, t5, t4
    li t6, 0x124e
    bne t5, t6, fail

    // Multiply/divide + independent ALU. The second result must survive a
    // multicycle division without early/duplicate retirement.
    TEST 7
    li t0, 35
    li t1, 4
    .balign 8
    mul t2, t0, t1
    addi t3, zero, 19
    add t4, t2, t3
    li t5, 159
    bne t4, t5, fail
    .balign 8
    div t2, t0, t1
    addi t3, zero, 9
    add t4, t2, t3
    li t5, 17
    bne t4, t5, fail

    // A branch in word0 must suppress word1 and any following wrong-path store.
    TEST 8
    li s2, 0
    sw zero, 8(s4)
    .balign 8
    beq zero, zero, taken0
    addi s2, s2, 1
    sw t1, 8(s4)
taken0:
    bnez s2, fail
    lw t0, 8(s4)
    bnez t0, fail
    // A branch in word1 must preserve the older ALU and suppress younger work.
    .balign 8
    addi s2, s2, 3
    beq zero, zero, taken1
    addi s2, s2, 7
    sw t1, 8(s4)
taken1:
    li t0, 3
    bne s2, t0, fail
    lw t0, 8(s4)
    bnez t0, fail

    // JAL/JALR at either fetch position; link is the address after that jump.
    TEST 9
    .balign 8
    jal t0, jal_target
jal_link:
    addi s2, s2, 13
jal_target:
    la t1, jal_link
    bne t0, t1, fail
    la t2, jalr_target
    .balign 8
    addi t3, zero, 5
    jalr t0, t2, 0
jalr_link:
    addi s2, s2, 17
jalr_target:
    la t1, jalr_link
    bne t0, t1, fail
    li t0, 5
    bne t3, t0, fail
    li t0, 3
    bne s2, t0, fail

    // Primary load fault cancels a paired younger ALU before register writeback.
    TEST 10
    li s2, 77
    EXPECT_TRAP 4, 1, fault_lw, after_lw
    .balign 8
fault_lw:
    lw t3, 1(zero)
    addi s2, zero, 55
after_lw:
    li t0, 77
    bne s2, t0, fail

    // Primary store bus fault must likewise cancel the second lane.
    TEST 11
    EXPECT_TRAP 7, 0x20000000, fault_sw, after_sw
    lui t2, 0x20000
    .balign 8
fault_sw:
    sw zero, 0(t2)
    addi s2, zero, 55
after_sw:
    li t0, 77
    bne s2, t0, fail

    // Illegal instruction in word1: word0 commits exactly once; word1 has
    // mepc = word0 + 4, and younger word2 is killed.
    TEST 12
    EXPECT_TRAP 2, 0xffffffff, fault_illegal, after_illegal
    .balign 8
    addi s2, s2, 1
fault_illegal:
    .word 0xffffffff
    addi s2, s2, 9
after_illegal:
    li t0, 78
    bne s2, t0, fail

    // ECALL in word1 must serialize and see the preceding lane's committed data.
    TEST 13
    EXPECT_TRAP 11, 0, fault_ecall, after_ecall
    .balign 8
    addi s2, s2, 1
fault_ecall:
    ecall
    addi s2, s2, 9
after_ecall:
    li t0, 79
    bne s2, t0, fail

    // A 64-bit fetch at 0xfffc contains one legal instruction and one invalid
    // address. The first instruction MUST commit before the access-fault trap.
    TEST 14
    li t0, 0xfffc
    li t1, 0x00190913             // addi s2, s2, 1
    sw t1, 0(t0)
    fence.i
    li s8, 1
    li s9, 0x10000
    li s10, 0x10000
    la s11, after_boundary
    jalr zero, t0, 0
after_boundary:
    li t0, 80
    bne s2, t0, fail
    li t0, 5
    bne s3, t0, fail

    // Exact retirement count between serialized CSR reads: the first CSRR
    // plus twelve ALU instructions have retired before the second read.
    TEST 15
    csrr s6, minstret
    addi t0, zero, 1
    addi t1, zero, 2
    addi t0, zero, 3
    addi t1, zero, 4
    addi t0, zero, 5
    addi t1, zero, 6
    addi t0, zero, 7
    addi t1, zero, 8
    addi t0, zero, 9
    addi t1, zero, 10
    addi t0, zero, 11
    addi t1, zero, 12
    csrr s7, minstret
    sub t0, s7, s6
    li t1, 13
    bne t0, t1, fail

    // A younger fault in the full lane must retain the older auxiliary ALU.
    // The preceding write creates WAW/RAW, keeping that ALU available to pair
    // with the faulting instruction instead of pairing with its predecessor.
    TEST 16
    EXPECT_TRAP 4, 1, younger_lw, resume_younger_lw
    li s2, 80
    addi s2, s2, 1
younger_lw:
    lw t3, 1(zero)
    addi s2, s2, 100
resume_younger_lw:
    li t0, 81
    bne s2, t0, fail

    TEST 17
    EXPECT_TRAP 7, 0x20000000, younger_sw, resume_younger_sw
    lui t2, 0x20000
    li s2, 81
    addi s2, s2, 1
younger_sw:
    sw zero, 0(t2)
    addi s2, s2, 100
resume_younger_sw:
    li t0, 82
    bne s2, t0, fail

    TEST 18
    EXPECT_TRAP 0, 0, younger_branch, resume_younger_branch
    la s9, younger_branch + 2
    li s2, 82
    addi s2, s2, 1
younger_branch:
    .word 0x00000163             // beq zero, zero, +2: taken, misaligned
    addi s2, s2, 100
resume_younger_branch:
    li t0, 83
    bne s2, t0, fail

    TEST 19
    EXPECT_TRAP 0, 2, younger_jalr, resume_younger_jalr
    li t2, 2
    li s2, 83
    addi s2, s2, 1
younger_jalr:
    jalr t3, t2, 0
    addi s2, s2, 100
resume_younger_jalr:
    li t0, 84
    bne s2, t0, fail
    li t0, 9
    bne s3, t0, fail

    lui t0, 0x10000
    sw zero, 4(t0)
1:  j 1b

.balign 4
trap_handler:
    csrr t0, mcause
    bne t0, s8, fail
    csrr t0, mtval
    bne t0, s9, fail
    csrr t0, mepc
    bne t0, s10, fail
    addi s3, s3, 1
    csrw mepc, s11
    mret

fail:
    lui t0, 0x10000
    sw gp, 4(t0)
2:  j 2b
