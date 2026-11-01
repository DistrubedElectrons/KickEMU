/*
 * KICK demo memory layout: one image in DDR, loaded by QEMU from the ELF
 * (-kernel kick-demo.elf). KICK_LOAD_ADDR comes from CMake (--defsym).
 */
ENTRY(_start)

SECTIONS
{
    . = KICK_LOAD_ADDR;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data : ALIGN(16) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    /* Level 1 translation table, 4 KiB aligned */
    .pagetable (NOLOAD) : ALIGN(4096) {
        __pagetable = .;
        . += 4096;
    }
    .stack (NOLOAD) : ALIGN(16) {
        . += 64 * 1024;
        __stack_top = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
