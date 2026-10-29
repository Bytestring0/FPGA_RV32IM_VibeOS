// Run the DDR checks, then execute and modify a warmed multi-line DDR function.
#define main ddr_baseline_main
#include "ddr_smoke.c"
#undef main
static int check_high_multiply(void) {
    static const uint32_t cases[][5]={
        {0x80000000u,0xffffffffu,0,0x80000000u,0x7fffffffu},
        {0xffffffffu,0xffffffffu,0,0xffffffffu,0xfffffffeu},
        {0x7fffffffu,0xffffffffu,0xffffffffu,0x7ffffffeu,0x7ffffffeu},
        {0x80000000u,0x80000000u,0x40000000u,0xc0000000u,0x40000000u}
    };
    for(unsigned k=0;k<4;++k) {
        uint32_t h,hsu,hu;
        __asm__ volatile("mulh %0,%1,%2":"=r"(h):"r"(cases[k][0]),"r"(cases[k][1]));
        __asm__ volatile("mulhsu %0,%1,%2":"=r"(hsu):"r"(cases[k][0]),"r"(cases[k][1]));
        __asm__ volatile("mulhu %0,%1,%2":"=r"(hu):"r"(cases[k][0]),"r"(cases[k][1]));
        if(h!=cases[k][2] || hsu!=cases[k][3] || hu!=cases[k][4]) {
            puts_uart("CPU MULH FAIL case=");puthex(k);puts_uart("\r\n");return 7;
        }
    }
    puts_uart("CPU MULH PASS signed mixed unsigned edges\r\n");return 0;
}
static int check_minmax(void) {
    static const uint32_t cases[][6]={
        {0x80000000u,0x7fffffffu,0x80000000u,0x7fffffffu,0x7fffffffu,0x80000000u},
        {0xffffffffu,0,0xffffffffu,0,0,0xffffffffu},
        {0x12345678u,0x12345678u,0x12345678u,0x12345678u,0x12345678u,0x12345678u}
    };
    for(unsigned k=0;k<3;++k) {
        uint32_t mn,mx,mnu,mxu;
        // Raw standard Zbb encodings keep the firmware compiler's RV32IM ABI.
        __asm__ volatile(".insn r 0x33,4,0x05,%0,%1,%2\n\taddi %0,%0,0":"=r"(mn):"r"(cases[k][0]),"r"(cases[k][1]));
        __asm__ volatile(".insn r 0x33,6,0x05,%0,%1,%2\n\taddi %0,%0,0":"=r"(mx):"r"(cases[k][0]),"r"(cases[k][1]));
        __asm__ volatile(".insn r 0x33,5,0x05,%0,%1,%2\n\taddi %0,%0,0":"=r"(mnu):"r"(cases[k][0]),"r"(cases[k][1]));
        __asm__ volatile(".insn r 0x33,7,0x05,%0,%1,%2\n\taddi %0,%0,0":"=r"(mxu):"r"(cases[k][0]),"r"(cases[k][1]));
        if(mn!=cases[k][2] || mx!=cases[k][3] || mnu!=cases[k][4] || mxu!=cases[k][5]) {
            puts_uart("CPU MINMAX FAIL case=");puthex(k);puts_uart("\r\n");return 8;
        }
    }
    puts_uart("CPU MINMAX PASS signed unsigned equal edges\r\n");return 0;
}
int main(void) {
    int rc=ddr_baseline_main(); if(rc) return rc;
    rc=check_high_multiply();if(rc)return rc;
    rc=check_minmax();if(rc)return rc;
    volatile uint32_t *code=(volatile uint32_t*)0x80010000u;
    code[0]=0x00000513u; // addi a0,zero,0
    for(uint32_t i=1;i<32;++i) code[i]=0x00150513u; // addi a0,a0,1
    code[32]=0x00008067u; // ret
    __asm__ volatile("fence.i" ::: "memory");
    int (*fn)(void)=(int(*)(void))0x80010000u;
    if(fn()!=31) return fail(0x80010000u,0,31);
    uint64_t start=read_cycles();
    for(uint32_t i=0;i<100;++i) if(fn()!=31) return fail(0x80010000u,i,31);
    uint32_t elapsed=(uint32_t)(read_cycles()-start);
    code[1]=0x00250513u; // change one already-cached instruction
    __asm__ volatile("fence.i" ::: "memory");
    int got=fn();if(got!=32) return fail(0x80010004u,(uint32_t)got,32);
    // Repeatedly patch the line most recently held by the instruction buffer.
    for(uint32_t k=0;k<16;++k) {
        code[32]=(k<<20)|0x00050513u; code[33]=0x00008067u;
        __asm__ volatile("fence.i" ::: "memory");
        got=fn();if(got!=(int)(32+k)) return fail(0x80010080u,(uint32_t)got,32+k);
    }
    if(MMIO32(0x10000200u)&32u) {
        MMIO32(0x10000204u)=1;
        start=read_cycles();
        while(MMIO32(0x10000204u)&1u)
            if(read_cycles()-start>(uint64_t)CPU_HZ*5) return 4;
        if(MMIO32(0x10000204u)&2u) return 5;
        if(!MMIO32(0x10000210u)||!MMIO32(0x10000218u)||!MMIO32(0x10000220u)||!MMIO32(0x10000224u)) return 6;
        puts_uart("CACHE I_HIT=");puthex(MMIO32(0x10000210u));
        puts_uart(" D_HIT=");puthex(MMIO32(0x10000218u));
        puts_uart(" WB=");puthex(MMIO32(0x10000220u));
        puts_uart(" IBUF_HIT=");puthex(MMIO32(0x10000224u));puts_uart("\r\n");
    }
    puts_uart("CACHE_WORK_CYCLES=");puthex(elapsed);puts_uart("\r\n");
    puts_uart("CACHE PASS DDR code warm-hit self-modify maintenance\r\n");
    return 0;
}
