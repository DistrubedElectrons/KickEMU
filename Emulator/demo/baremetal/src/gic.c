/*
 * GICv3 driver (Arm GICv3/v4 architecture specification, IHI 0069).
 *
 * The i.MX8MM has a GIC-500 with the distributor at 0x38800000 and one
 * 128 KiB redistributor frame per core from 0x38880000. Without EL3
 * firmware the GIC runs with a single security state (GICD_CTLR.DS = 1),
 * so the demo uses Group 1 interrupts signalled as IRQ, affinity routing
 * enabled, and the system register CPU interface.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gic.h"
#include "imx8mm.h"

#define GICD_CTLR           0x0000
#define GICD_IGROUPR(n)     (0x0080 + 4 * ((n) / 32))
#define GICD_ISENABLER(n)   (0x0100 + 4 * ((n) / 32))
#define GICD_IPRIORITYR(n)  (0x0400 + (n))
#define GICD_ICFGR(n)       (0x0c00 + 4 * ((n) / 16))
#define GICD_IROUTER(n)     (0x6000 + 8 * (n))

#define GICD_CTLR_ENGRP1    (1u << 1)
#define GICD_CTLR_ARE       (1u << 4)
#define GICD_CTLR_RWP       (1u << 31)

#define GICR_WAKER          0x0014
#define GICR_WAKER_PSLEEP   (1u << 1)
#define GICR_WAKER_CASLEEP  (1u << 2)

#define MAX_IRQ             160

static irq_fn handlers[MAX_IRQ];
static volatile unsigned long irq_count;

#define ICC_SRE_EL1(v)      __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(v))
#define ICC_PMR_EL1(v)      __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(v))
#define ICC_BPR1_EL1(v)     __asm__ volatile("msr S3_0_C12_C12_3, %0" :: "r"(v))
#define ICC_IGRPEN1_EL1(v)  __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"(v))
#define ICC_EOIR1_EL1(v)    __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"(v))
#define ISB()               __asm__ volatile("isb" ::: "memory")

static uint64_t icc_iar1(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v));
    return v;
}

void gic_init(void)
{
    uint64_t one = 1, sre = 7, pmr = 0xff, zero = 0;

    /* Distributor: affinity routing, Group 1 enabled */
    wr32(GICD_BASE + GICD_CTLR, 0);
    while (rd32(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP) {
    }
    wr32(GICD_BASE + GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_ENGRP1);
    while (rd32(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP) {
    }

    /* Redistributor of CPU 0: wake it up */
    wr32(GICR_BASE + GICR_WAKER,
         rd32(GICR_BASE + GICR_WAKER) & ~GICR_WAKER_PSLEEP);
    while (rd32(GICR_BASE + GICR_WAKER) & GICR_WAKER_CASLEEP) {
    }

    /* CPU interface: system registers, all priorities, Group 1 on */
    ICC_SRE_EL1(sre);
    ISB();
    ICC_PMR_EL1(pmr);
    ICC_BPR1_EL1(zero);
    ICC_IGRPEN1_EL1(one);
    ISB();
}

void gic_enable_spi(unsigned intid, uint8_t prio, irq_fn fn)
{
    uint32_t bit = 1u << (intid % 32);
    uint32_t cfg;

    handlers[intid] = fn;
    wr32(GICD_BASE + GICD_IGROUPR(intid),
         rd32(GICD_BASE + GICD_IGROUPR(intid)) | bit);
    *(volatile uint8_t *)(uintptr_t)(GICD_BASE + GICD_IPRIORITYR(intid)) =
        prio;
    /* level sensitive, as every i.MX8MM peripheral interrupt */
    cfg = rd32(GICD_BASE + GICD_ICFGR(intid));
    cfg &= ~(3u << (2 * (intid % 16)));
    wr32(GICD_BASE + GICD_ICFGR(intid), cfg);
    /* route to affinity 0.0.0.0 (CPU 0) */
    *(volatile uint64_t *)(uintptr_t)(GICD_BASE + GICD_IROUTER(intid)) = 0;
    wr32(GICD_BASE + GICD_ISENABLER(intid), bit);
}

void irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

void irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

unsigned long gic_irq_count(void)
{
    return irq_count;
}

/* Called from the vector table (start.S) */
void irq_handler(void)
{
    uint64_t iar = icc_iar1();
    unsigned intid = iar & 0xffffff;

    if (intid < MAX_IRQ && handlers[intid]) {
        handlers[intid]();
        irq_count++;
    }
    if (intid < 1020) {
        ICC_EOIR1_EL1(iar);
    }
}
