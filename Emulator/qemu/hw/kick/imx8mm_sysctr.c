/*
 * i.MX 8M Mini System Counter (SYS_CTR)
 *
 * IMX8MMRM chapter 12.2 "System Counter (SYS_CTR)". The system counter is
 * the 56-bit time base that feeds the Cortex-A53 generic timers. It has
 * three 64 KiB frames:
 *
 *   read frame    (0x306a0000)  CNTCV_LO 0x08, CNTCV_HI 0x0c, CNTID 0xfd0
 *   compare frame (0x306b0000)  per comparator n at n * 0x100:
 *                               CMPCV_LO 0x20, CMPCV_HI 0x24,
 *                               CMPCR 0x2c: EN [0], IMASK [1], ISTAT [2]
 *   control frame (0x306c0000)  CNTCR 0x00: EN [0], HDBG [1], FCREQ [9:8]
 *                               CNTSR 0x04: DBGH [1], FCA [9:8]
 *                               CNTCV_LO/HI 0x08/0x0c (writable when off)
 *                               CNTFID0..2 0x20..0x28, CNTID 0xfd0
 *
 * Linux uses comparator 0 as a broadcast clockevent (timer-imx-sysctr.c,
 * IRQ 47). The counter runs on QEMU_CLOCK_VIRTUAL at CNTFID0 (8 MHz, the
 * 24 MHz oscillator / 3), the same rate the SoC programs into the CPUs'
 * CNTFRQ, so CNTCV and the A53 CNTPCT agree under TCG. Under HVF the CPU
 * counter is the host's and runs at the host frequency.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#include "imx8mm_sysctr.h"

#define CNTCR           0x00
#define CNTSR           0x04
#define CNTCV_LO        0x08
#define CNTCV_HI        0x0c
#define CNTFID0         0x20
#define CNTFID1         0x24
#define CNTFID2         0x28
#define CNTID0          0xfd0

#define CMPCV_LO        0x20
#define CMPCV_HI        0x24
#define CMPCR           0x2c
#define CMP_STRIDE      0x100

#define CNTCR_EN        BIT(0)
#define CNTCR_HDBG      BIT(1)
#define CNTCR_FCR0      BIT(8)
#define CNTCR_FCR1      BIT(9)

#define CMPCR_EN        BIT(0)
#define CMPCR_IMASK     BIT(1)
#define CMPCR_ISTAT     BIT(2)

/* 56-bit counter */
#define CNT_MASK        MAKE_64BIT_MASK(0, 56)

static uint32_t imx8mm_sysctr_freq(IMX8MMSysCtrState *s)
{
    /* FCREQ selects the frequency table entry, entry 0 when neither is set */
    return (s->cntcr & CNTCR_FCR1) ? s->fid[1] : s->fid[0];
}

static bool imx8mm_sysctr_running(IMX8MMSysCtrState *s)
{
    return (s->cntcr & CNTCR_EN) && imx8mm_sysctr_freq(s);
}

static uint64_t imx8mm_sysctr_count(IMX8MMSysCtrState *s)
{
    int64_t now;

    if (!imx8mm_sysctr_running(s)) {
        return s->base_cnt;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return (s->base_cnt + muldiv64(now - s->base_ns, imx8mm_sysctr_freq(s),
                                   NANOSECONDS_PER_SECOND)) & CNT_MASK;
}

/* Fold elapsed time into base_cnt before the rate or run state changes */
static void imx8mm_sysctr_rebase(IMX8MMSysCtrState *s)
{
    s->base_cnt = imx8mm_sysctr_count(s);
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void imx8mm_sysctr_update_cmp(IMX8MMSysCtrState *s, int n)
{
    IMX8MMSysCtrCmp *c = &s->cmp[n];
    uint64_t cnt = imx8mm_sysctr_count(s);

    if ((c->cmpcr & CMPCR_EN) && cnt >= c->cmpcv) {
        c->cmpcr |= CMPCR_ISTAT;
    } else {
        c->cmpcr &= ~CMPCR_ISTAT;
    }
    qemu_set_irq(c->irq, (c->cmpcr & CMPCR_ISTAT) &&
                         !(c->cmpcr & CMPCR_IMASK));

    /* Arm the timer for a future match */
    if ((c->cmpcr & CMPCR_EN) && !(c->cmpcr & CMPCR_ISTAT) &&
        imx8mm_sysctr_running(s)) {
        int64_t delta_ns = muldiv64(c->cmpcv - cnt, NANOSECONDS_PER_SECOND,
                                    imx8mm_sysctr_freq(s));

        timer_mod(c->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            MAX(delta_ns, 1));
    } else {
        timer_del(c->timer);
    }
}

static void imx8mm_sysctr_update_all(IMX8MMSysCtrState *s)
{
    for (int n = 0; n < IMX8MM_SYSCTR_NUM_CMP; n++) {
        imx8mm_sysctr_update_cmp(s, n);
    }
}

static void imx8mm_sysctr_cmp_tick0(void *opaque)
{
    imx8mm_sysctr_update_cmp(opaque, 0);
}

static void imx8mm_sysctr_cmp_tick1(void *opaque)
{
    imx8mm_sysctr_update_cmp(opaque, 1);
}

static uint64_t imx8mm_sysctr_id(hwaddr offset)
{
    /* CNTID0: counter frequency switching implemented, 2 table entries */
    return offset == CNTID0 ? 0x00000001 : 0;
}

static uint64_t imx8mm_sysctr_rd_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IMX8MMSysCtrState *s = IMX8MM_SYSCTR(opaque);

    switch (offset) {
    case CNTCV_LO:
        return (uint32_t)imx8mm_sysctr_count(s);
    case CNTCV_HI:
        return imx8mm_sysctr_count(s) >> 32;
    case CNTID0:
        return imx8mm_sysctr_id(offset);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read frame offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_SYSCTR, offset);
        return 0;
    }
}

static void imx8mm_sysctr_rd_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: read frame is read-only (0x%"
                  HWADDR_PRIx ")\n", TYPE_IMX8MM_SYSCTR, offset);
}

static uint64_t imx8mm_sysctr_cmp_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    IMX8MMSysCtrState *s = IMX8MM_SYSCTR(opaque);
    int n = offset / CMP_STRIDE;
    hwaddr reg = offset % CMP_STRIDE;
    IMX8MMSysCtrCmp *c;

    if (n >= IMX8MM_SYSCTR_NUM_CMP) {
        if (offset == CNTID0) {
            return imx8mm_sysctr_id(offset);
        }
        goto bad;
    }
    c = &s->cmp[n];
    switch (reg) {
    case CMPCV_LO:
        return (uint32_t)c->cmpcv;
    case CMPCV_HI:
        return c->cmpcv >> 32;
    case CMPCR:
        imx8mm_sysctr_update_cmp(s, n);
        return c->cmpcr;
    }
bad:
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad compare frame offset 0x%"
                  HWADDR_PRIx "\n", TYPE_IMX8MM_SYSCTR, offset);
    return 0;
}

static void imx8mm_sysctr_cmp_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    IMX8MMSysCtrState *s = IMX8MM_SYSCTR(opaque);
    int n = offset / CMP_STRIDE;
    hwaddr reg = offset % CMP_STRIDE;
    IMX8MMSysCtrCmp *c;

    if (n >= IMX8MM_SYSCTR_NUM_CMP) {
        goto bad;
    }
    c = &s->cmp[n];
    switch (reg) {
    case CMPCV_LO:
        c->cmpcv = deposit64(c->cmpcv, 0, 32, value);
        break;
    case CMPCV_HI:
        /* Only 24 bits: the counter is 56 bits wide */
        c->cmpcv = deposit64(c->cmpcv, 32, 24, value);
        break;
    case CMPCR:
        /* ISTAT is read-only status */
        c->cmpcr = value & (CMPCR_EN | CMPCR_IMASK);
        break;
    default:
        goto bad;
    }
    imx8mm_sysctr_update_cmp(s, n);
    return;
bad:
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad compare frame offset 0x%"
                  HWADDR_PRIx "\n", TYPE_IMX8MM_SYSCTR, offset);
}

static uint64_t imx8mm_sysctr_ctrl_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    IMX8MMSysCtrState *s = IMX8MM_SYSCTR(opaque);

    switch (offset) {
    case CNTCR:
        return s->cntcr;
    case CNTSR:
        /* FCA mirrors the accepted request, DBGH the halt-on-debug state */
        return (s->cntcr & (CNTCR_FCR0 | CNTCR_FCR1)) |
               ((s->cntcr & CNTCR_HDBG) ? BIT(1) : 0);
    case CNTCV_LO:
        return (uint32_t)imx8mm_sysctr_count(s);
    case CNTCV_HI:
        return imx8mm_sysctr_count(s) >> 32;
    case CNTFID0:
        return s->fid[0];
    case CNTFID1:
        return s->fid[1];
    case CNTFID2:
        return 0;   /* end of the frequency table */
    case CNTID0:
        return imx8mm_sysctr_id(offset);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad control frame offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_SYSCTR, offset);
        return 0;
    }
}

static void imx8mm_sysctr_ctrl_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    IMX8MMSysCtrState *s = IMX8MM_SYSCTR(opaque);

    switch (offset) {
    case CNTCR:
        imx8mm_sysctr_rebase(s);
        s->cntcr = value & (CNTCR_EN | CNTCR_HDBG | CNTCR_FCR0 | CNTCR_FCR1);
        break;
    case CNTCV_LO:
        imx8mm_sysctr_rebase(s);
        s->base_cnt = deposit64(s->base_cnt, 0, 32, value);
        break;
    case CNTCV_HI:
        imx8mm_sysctr_rebase(s);
        s->base_cnt = deposit64(s->base_cnt, 32, 24, value);
        break;
    case CNTFID0:
    case CNTFID1:
        /* The frequency table is fixed by the integration */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: CNTFID is read-only\n",
                      TYPE_IMX8MM_SYSCTR);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad control frame offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_SYSCTR, offset);
        return;
    }
    imx8mm_sysctr_update_all(s);
}

#define SYSCTR_OPS(name)                                    \
    static const MemoryRegionOps imx8mm_sysctr_##name##_ops = { \
        .read = imx8mm_sysctr_##name##_read,                \
        .write = imx8mm_sysctr_##name##_write,              \
        .endianness = DEVICE_LITTLE_ENDIAN,                 \
        .valid = { .min_access_size = 4, .max_access_size = 4 }, \
    }

SYSCTR_OPS(rd);
SYSCTR_OPS(cmp);
SYSCTR_OPS(ctrl);

static void imx8mm_sysctr_reset(DeviceState *dev)
{
    IMX8MMSysCtrState *s = IMX8MM_SYSCTR(dev);

    /*
     * ATF enables the counter (CNTCR = EN | HDBG); when no firmware runs
     * before the payload, the board asks for that state at reset.
     */
    s->cntcr = s->start_enabled ? (CNTCR_EN | CNTCR_HDBG) : 0;
    s->base_cnt = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (int n = 0; n < IMX8MM_SYSCTR_NUM_CMP; n++) {
        s->cmp[n].cmpcv = 0;
        s->cmp[n].cmpcr = 0;
    }
    imx8mm_sysctr_update_all(s);
}

static void imx8mm_sysctr_init(Object *obj)
{
    IMX8MMSysCtrState *s = IMX8MM_SYSCTR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem_rd, obj, &imx8mm_sysctr_rd_ops, s,
                          TYPE_IMX8MM_SYSCTR ".rd", IMX8MM_SYSCTR_FRAME_SIZE);
    memory_region_init_io(&s->iomem_cmp, obj, &imx8mm_sysctr_cmp_ops, s,
                          TYPE_IMX8MM_SYSCTR ".cmp", IMX8MM_SYSCTR_FRAME_SIZE);
    memory_region_init_io(&s->iomem_ctrl, obj, &imx8mm_sysctr_ctrl_ops, s,
                          TYPE_IMX8MM_SYSCTR ".ctrl",
                          IMX8MM_SYSCTR_FRAME_SIZE);
    sysbus_init_mmio(sbd, &s->iomem_rd);
    sysbus_init_mmio(sbd, &s->iomem_cmp);
    sysbus_init_mmio(sbd, &s->iomem_ctrl);

    for (int n = 0; n < IMX8MM_SYSCTR_NUM_CMP; n++) {
        sysbus_init_irq(sbd, &s->cmp[n].irq);
    }
    s->cmp[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   imx8mm_sysctr_cmp_tick0, s);
    s->cmp[1].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   imx8mm_sysctr_cmp_tick1, s);
}

static const VMStateDescription imx8mm_sysctr_cmp_vmstate = {
    .name = TYPE_IMX8MM_SYSCTR ".cmp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(cmpcv, IMX8MMSysCtrCmp),
        VMSTATE_UINT32(cmpcr, IMX8MMSysCtrCmp),
        VMSTATE_TIMER_PTR(timer, IMX8MMSysCtrCmp),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription imx8mm_sysctr_vmstate = {
    .name = TYPE_IMX8MM_SYSCTR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cntcr, IMX8MMSysCtrState),
        VMSTATE_UINT64(base_cnt, IMX8MMSysCtrState),
        VMSTATE_INT64(base_ns, IMX8MMSysCtrState),
        VMSTATE_STRUCT_ARRAY(cmp, IMX8MMSysCtrState, IMX8MM_SYSCTR_NUM_CMP,
                             1, imx8mm_sysctr_cmp_vmstate, IMX8MMSysCtrCmp),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx8mm_sysctr_properties[] = {
    /* Base frequency: 24 MHz / 3; alternate: the 32 kHz oscillator */
    DEFINE_PROP_UINT32("cntfid0", IMX8MMSysCtrState, fid[0], 8000000),
    DEFINE_PROP_UINT32("cntfid1", IMX8MMSysCtrState, fid[1], 32768),
    DEFINE_PROP_BOOL("start-enabled", IMX8MMSysCtrState, start_enabled,
                     false),
};

static void imx8mm_sysctr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_sysctr_reset);
    device_class_set_props(dc, imx8mm_sysctr_properties);
    dc->vmsd = &imx8mm_sysctr_vmstate;
    dc->desc = "i.MX8MM System Counter";
}

static const TypeInfo imx8mm_sysctr_types[] = {
    {
        .name          = TYPE_IMX8MM_SYSCTR,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMSysCtrState),
        .instance_init = imx8mm_sysctr_init,
        .class_init    = imx8mm_sysctr_class_init,
    },
};

DEFINE_TYPES(imx8mm_sysctr_types)
