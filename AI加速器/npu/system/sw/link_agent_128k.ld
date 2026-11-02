/* Agent application profile: requires ag_npu_system.TCM_WORDS=32768.
 * Default CPU/sw/link.ld and 64KiB system profile are unchanged. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 128K }
SECTIONS {
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data : { *(.data .data.*) . = ALIGN(4); } > RAM
    .sdata : { __global_pointer$ = . + 0x800; *(.sdata .sdata.*) } > RAM
    .bss (NOLOAD) : {
        . = ALIGN(4); __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4); __bss_end = .;
    } > RAM
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
    ASSERT(__bss_end < __stack_top - 8192, "Agent TCM overflow: reserve 8 KiB for stack")
}
