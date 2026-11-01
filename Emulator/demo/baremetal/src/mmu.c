/*
 * Stage 1 translation at EL1: a single level-1 table with 1 GiB blocks
 * covering the 4 GiB the i.MX8MM decodes below the 64-bit DDR alias.
 *
 *   0x0000_0000 - 0x3fff_ffff  Device-nGnRnE, never executable (ROM,
 *                              OCRAM, all peripherals, GIC)
 *   0x4000_0000 - 0xffff_ffff  Normal, write-back cacheable (DDR)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mmu.h"

#define BLOCK               0x1ull
#define AF                  (1ull << 10)
#define SH_INNER            (3ull << 8)
#define ATTR(n)             ((uint64_t)(n) << 2)
#define PXN                 (1ull << 53)
#define UXN                 (1ull << 54)

#define MAIR_DEVICE         0x00    /* attr 0: Device-nGnRnE */
#define MAIR_NORMAL_WB      0xff    /* attr 1: Normal, WB RW-allocate */

/* TCR: 4 GiB VA (T0SZ 32), 4 KiB granule, inner/outer WB, 40-bit PA */
#define TCR_VALUE           (32ull | (1ull << 8) | (1ull << 10) | \
                             (3ull << 12) | (1ull << 23) | (2ull << 32))

#define SCTLR_M             (1ull << 0)
#define SCTLR_C             (1ull << 2)
#define SCTLR_I             (1ull << 12)

extern uint64_t __pagetable[];

void mmu_enable(void)
{
    uint64_t *l1 = __pagetable;
    uint64_t sctlr;

    l1[0] = 0x00000000ull | BLOCK | AF | ATTR(0) | PXN | UXN;
    for (int i = 1; i < 4; i++) {
        l1[i] = ((uint64_t)i << 30) | BLOCK | AF | SH_INNER | ATTR(1);
    }

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "dsb ish\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :: "r"((uint64_t)MAIR_DEVICE | ((uint64_t)MAIR_NORMAL_WB << 8)),
           "r"((uint64_t)TCR_VALUE), "r"(l1)
        : "memory");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile("msr sctlr_el1, %0\nisb" :: "r"(sctlr) : "memory");
}

void cache_clean_range(const void *start, uint64_t len)
{
    uint64_t ctr, line;
    uint64_t p = (uint64_t)start;
    uint64_t end = p + len;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = 4ull << ((ctr >> 16) & 0xf);     /* DminLine, in words */
    for (p &= ~(line - 1); p < end; p += line) {
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
