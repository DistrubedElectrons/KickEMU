/*
 * Entry, exception level setup and exception vectors.
 *
 * QEMU starts the image on CPU 0 in the highest exception level the
 * firmware substitute leaves to the payload: EL2 under TCG, EL1 under HVF
 * (the real chain enters U-Boot/Linux at EL2 from ATF). The demo always
 * runs at EL1, so from EL2 it hands the timer, the GIC CPU interface and
 * AArch64 state to EL1 and drops down.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define SCTLR_EL1_RES1  0x30d00800
#define HCR_EL2_RW      (1 << 31)
#define SPSR_EL1H_MASKED 0x3c5      /* EL1h, D, A, I, F masked */

/* ICC_SRE_EL2: SRE | DFB | DIB | Enable (EL1 may use the sysreg interface) */
#define ICC_SRE_EL2     S3_4_C12_C9_5

    .section .text.boot, "ax"
    .global _start
_start:
    /* Only CPU 0 runs the demo; others (if released) sleep */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, park

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    from_el2
    cmp     x0, #1
    b.eq    el1_entry
    /* EL3 is not supported: no secure GIC setup is done here */
    b       park

from_el2:
    mov     x0, #HCR_EL2_RW
    msr     hcr_el2, x0
    mov     x0, #3                  /* EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0xf
    msr     ICC_SRE_EL2, x0
    isb
    ldr     x0, =SCTLR_EL1_RES1
    msr     sctlr_el1, x0
    mov     x0, #SPSR_EL1H_MASKED
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:
    ldr     x0, =__stack_top
    mov     sp, x0
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    /* Clear .bss */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    bl      main
park:
    wfe
    b       park

/* Save the caller-saved registers, call into C, restore and return */
.macro EXC_ENTRY handler
    sub     sp, sp, #(20 * 8)
    stp     x0, x1, [sp, #(0 * 8)]
    stp     x2, x3, [sp, #(2 * 8)]
    stp     x4, x5, [sp, #(4 * 8)]
    stp     x6, x7, [sp, #(6 * 8)]
    stp     x8, x9, [sp, #(8 * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x30, [sp, #(18 * 8)]
    bl      \handler
    ldp     x0, x1, [sp, #(0 * 8)]
    ldp     x2, x3, [sp, #(2 * 8)]
    ldp     x4, x5, [sp, #(4 * 8)]
    ldp     x6, x7, [sp, #(6 * 8)]
    ldp     x8, x9, [sp, #(8 * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x30, [sp, #(18 * 8)]
    add     sp, sp, #(20 * 8)
    eret
.endm

/* Exception vector table: 16 entries of 0x80 bytes */
    .text
    .balign 2048
vectors:
    .balign 0x80
    b       exc_fatal               /* EL1t sync */
    .balign 0x80
    b       exc_fatal               /* EL1t IRQ */
    .balign 0x80
    b       exc_fatal               /* EL1t FIQ */
    .balign 0x80
    b       exc_fatal               /* EL1t SError */
    .balign 0x80
    b       exc_fatal               /* EL1h sync */
    .balign 0x80
    b       irq_entry               /* EL1h IRQ */
    .balign 0x80
    b       exc_fatal               /* EL1h FIQ */
    .balign 0x80
    b       exc_fatal               /* EL1h SError */
    .rept 8
    .balign 0x80
    b       exc_fatal               /* lower EL, not used */
    .endr

irq_entry:
    EXC_ENTRY irq_handler

exc_fatal:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      fatal_exception
    b       park
