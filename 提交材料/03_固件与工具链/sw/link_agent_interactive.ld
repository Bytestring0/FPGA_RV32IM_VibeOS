/* 64KiB TCM boot/hot code; SD-loaded DDR Agent/UI cold code. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
 APPDDR (rwx) : ORIGIN = 0xbf000000, LENGTH = 1M }
SECTIONS {
    .boot 0x00000000 : {
        KEEP(*(.text.start))
        *agent_ddr_npu_tensor_reference.o(.text.npu_tensor_query .text.native_overlap .text.span)
        *agent_ddr_npu_raw.o(.text.npu_raw_query .text.region_table_valid .text.owned .text.overlap)
        *agent_ddr_npu_fixed_math.o(.text.decode_to_q16 .text.flag .text.counter_add .text.npu_fixed_fp16_bits_to_q16 .text.npu_fixed_bf16_bits_to_q16 .text.npu_fixed_f32_bits_to_q16)
        *agent_ddr_qw_int_sample.o(.text.qw_sample_greedy_bits)
    } > RAM
    .ddr_resources 0xbf000000 : {
        __agent_resource_start = .; KEEP(*(.agent_profile)) *(.rodata.npu_fixed_lut_*)
        *agent_ddr_*.o(.text .text.* .rodata .rodata.*)
        . = ALIGN(32); __agent_resource_end = .;
    } > APPDDR
    .ddr_bss (NOLOAD) : { . = ALIGN(32); __ddr_bss_start = .; *agent_ddr_core_*.o(.bss .bss.* COMMON) *agent_ddr_ag_coremark_ui.o(.bss .bss.* COMMON) . = ALIGN(32); __ddr_bss_end = .; } > APPDDR
    .text : { *(.text .text.*) } > RAM
    .rodata : { *(.resource_pin) *(.rodata .rodata.*) } > RAM
    .data : { *(.data .data.*) . = ALIGN(4); } > RAM
    .sdata : { __global_pointer$ = . + 0x800; *(.sdata .sdata.*) } > RAM
    .bss (NOLOAD) : {
        . = ALIGN(4); __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4); __bss_end = .;
    } > RAM
    ASSERT(SIZEOF(.ddr_resources) <= 1048576, "App resource reserve overflow")
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
    ASSERT(__bss_end < __stack_top - 8192, "Agent TCM overflow: reserve 8 KiB for stack")
}