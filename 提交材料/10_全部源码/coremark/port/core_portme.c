#include "coremark.h"
#include "../platform.h"
#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415, seed2_volatile = 0x3415;
#else
volatile ee_s32 seed1_volatile = 0, seed2_volatile = 0;
#endif
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;
ee_u32 default_num_contexts = 1;
static CORE_TICKS begin, end;
void start_time(void) { begin = read_cycles(); }
void stop_time(void) { end = read_cycles(); }
CORE_TICKS get_time(void) { return end - begin; }
secs_ret time_in_secs(CORE_TICKS t) { return (secs_ret)(t / CPU_HZ); }
void portable_init(core_portable *p, int *argc, char *argv[]) {
    (void)argc; (void)argv; p->portable_id = 1;
}
void portable_fini(core_portable *p) {
    CORE_TICKS ticks = get_time();
    puts_uart("MEASURED_CYCLES_HEX=");
    puthex((uint32_t)(ticks >> 32)); puthex((uint32_t)ticks);
    puts_uart("\n");
    p->portable_id = 0;
}
