#ifndef AG_RISCV_TEST_H
#define AG_RISCV_TEST_H
#define RVTEST_RV32U
#define RVTEST_RV64U
#define TESTNUM gp
#define RVTEST_CODE_BEGIN \
    .section .text.start; .globl _start; _start:; \
    la t0, unexpected_trap; csrw mtvec, t0;
#define RVTEST_CODE_END \
    .balign 4; unexpected_trap:; csrr gp, mcause; ori gp, gp, 128; \
    lui t0, 0x10000; sw gp, 4(t0); 1: j 1b;
#define RVTEST_PASS lui t0, 0x10000; sw zero, 4(t0); 1: j 1b;
#define RVTEST_FAIL lui t0, 0x10000; sw TESTNUM, 4(t0); 1: j 1b;
#define RVTEST_DATA_BEGIN .balign 16; begin_signature:
#define RVTEST_DATA_END end_signature:
#endif
