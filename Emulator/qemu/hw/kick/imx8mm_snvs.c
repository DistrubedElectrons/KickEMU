/*
 * i.MX 8M Mini Secure Non-Volatile Storage (SNVS_HP / SNVS_LP)
 *
 * IMX8MMRM chapter 6.4 "Secure Non-Volatile Storage (SNVS)". The upstream
 * imx7_snvs model only covers the LP RTC counter and power off; Linux on
 * the KICK also uses the ON/OFF button (snvs_pwrkey, KEY_STOP in the KICK
 * firmware), the RTC alarm (rtc-snvs) and the status registers, so this
 * model implements:
 *
 *   HPSR   0x14  BTN [6] button level, BI [7] button interrupt (W1C)
 *   HPCR   0x08  RTC_EN [0] starts the HP real time counter
 *   HPRTCMR/LR 0x24/0x28  HP RTC, 47 bits of 32.768 kHz ticks
 *   LPCR   0x38  SRTC_ENV [0], LPTA_EN [1], LPWUI_EN [3], DP_EN [5],
 *                TOP [6], BTN_PRESS_TIME [17:16]
 *   LPSR   0x4c  LPTA [0] alarm, SPO [18] set-power-off request; W1C
 *   LPSRTCMR/LR 0x50/0x54  secure RTC, 47 bits of 32.768 kHz ticks
 *   LPTAR  0x58  alarm time in seconds (compared with SRTC >> 15)
 *   LPGPR0..3 0x90..0x9c  general purpose (battery backed)
 *
 * The button raises SPO (and IRQ 4) on press, like a short push on the
 * real part; the 5 s long-press hardware power off is not modelled.
 * Writing DP_EN | TOP to LPCR turns the system off.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "system/rtc.h"
#include "system/system.h"
#include "system/runstate.h"

#include "imx8mm_snvs.h"

#define SNVS_HPCOMR     0x04
#define SNVS_HPCR       0x08
#define SNVS_HPSR       0x14
#define SNVS_HPRTCMR    0x24
#define SNVS_HPRTCLR    0x28
#define SNVS_HPTAMR     0x2c
#define SNVS_HPTALR     0x30
#define SNVS_LPCR       0x38
#define SNVS_LPSR       0x4c
#define SNVS_LPSRTCMR   0x50
#define SNVS_LPSRTCLR   0x54
#define SNVS_LPTAR      0x58
#define SNVS_LPGPR3     0x9c

#define HPCR_RTC_EN     BIT(0)
#define HPSR_BTN        BIT(6)
#define HPSR_BI         BIT(7)

#define LPCR_SRTC_ENV   BIT(0)
#define LPCR_LPTA_EN    BIT(1)
#define LPCR_LPWUI_EN   BIT(3)
#define LPCR_DP_EN      BIT(5)
#define LPCR_TOP        BIT(6)

#define LPSR_LPTA       BIT(0)
#define LPSR_SPO        BIT(18)
#define LPSR_W1C_MASK   0xffffffff

#define RTC_FREQ        32768
#define RTC_MASK        MAKE_64BIT_MASK(0, 47)

#define R(off)          ((off) / 4)

static int64_t imx8mm_snvs_ticks(void)
{
    return muldiv64(qemu_clock_get_ns(rtc_clock), RTC_FREQ,
                    NANOSECONDS_PER_SECOND);
}

static uint64_t imx8mm_snvs_lp_count(IMX8MMSNVSState *s)
{
    if (!(s->regs[R(SNVS_LPCR)] & LPCR_SRTC_ENV)) {
        return s->lp_held;
    }
    return (s->lp_offset + imx8mm_snvs_ticks()) & RTC_MASK;
}

static uint64_t imx8mm_snvs_hp_count(IMX8MMSNVSState *s)
{
    if (!(s->regs[R(SNVS_HPCR)] & HPCR_RTC_EN)) {
        return s->hp_held;
    }
    return (s->hp_offset + imx8mm_snvs_ticks()) & RTC_MASK;
}

static void imx8mm_snvs_update_irq(IMX8MMSNVSState *s)
{
    uint32_t lpsr = s->regs[R(SNVS_LPSR)];
    uint32_t lpcr = s->regs[R(SNVS_LPCR)];

    qemu_set_irq(s->irq_rtc, (lpsr & LPSR_LPTA) && (lpcr & LPCR_LPTA_EN) &&
                             (lpcr & LPCR_LPWUI_EN));
    qemu_set_irq(s->irq_pwrkey, !!(lpsr & LPSR_SPO));
}

/* Compare the secure RTC (in seconds) against LPTAR and rearm the timer */
static void imx8mm_snvs_check_alarm(IMX8MMSNVSState *s)
{
    uint32_t lpcr = s->regs[R(SNVS_LPCR)];
    uint64_t now_ticks, alarm_ticks;

    timer_del(s->alarm_timer);
    if (!(lpcr & LPCR_LPTA_EN) || !(lpcr & LPCR_SRTC_ENV)) {
        imx8mm_snvs_update_irq(s);
        return;
    }

    now_ticks = imx8mm_snvs_lp_count(s);
    alarm_ticks = (uint64_t)s->regs[R(SNVS_LPTAR)] << 15;
    if (now_ticks >= alarm_ticks) {
        s->regs[R(SNVS_LPSR)] |= LPSR_LPTA;
    } else {
        int64_t delta = muldiv64(alarm_ticks - now_ticks,
                                 NANOSECONDS_PER_SECOND, RTC_FREQ);

        timer_mod(s->alarm_timer, qemu_clock_get_ns(rtc_clock) + delta);
    }
    imx8mm_snvs_update_irq(s);
}

static void imx8mm_snvs_alarm_cb(void *opaque)
{
    imx8mm_snvs_check_alarm(opaque);
}

static void imx8mm_snvs_set_button(IMX8MMSNVSState *s, bool pressed)
{
    if (pressed && !s->button) {
        /* Press: SPO request and button interrupt status */
        s->regs[R(SNVS_LPSR)] |= LPSR_SPO;
        s->regs[R(SNVS_HPSR)] |= HPSR_BI;
    }
    s->button = pressed;
    imx8mm_snvs_update_irq(s);
}

static void imx8mm_snvs_pwrkey_in(void *opaque, int n, int level)
{
    imx8mm_snvs_set_button(opaque, level);
}

static uint64_t imx8mm_snvs_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMSNVSState *s = IMX8MM_SNVS(opaque);

    switch (offset) {
    case SNVS_HPSR:
        return (s->regs[R(offset)] & ~HPSR_BTN) | (s->button ? HPSR_BTN : 0);
    case SNVS_HPRTCMR:
        return extract64(imx8mm_snvs_hp_count(s), 32, 15);
    case SNVS_HPRTCLR:
        return (uint32_t)imx8mm_snvs_hp_count(s);
    case SNVS_LPSRTCMR:
        return extract64(imx8mm_snvs_lp_count(s), 32, 15);
    case SNVS_LPSRTCLR:
        return (uint32_t)imx8mm_snvs_lp_count(s);
    default:
        if (offset <= SNVS_LPGPR3) {
            return s->regs[R(offset)];
        }
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_SNVS, offset);
        return 0;
    }
}

static void imx8mm_snvs_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    IMX8MMSNVSState *s = IMX8MM_SNVS(opaque);
    uint64_t cnt;

    switch (offset) {
    case SNVS_HPCR:
        cnt = imx8mm_snvs_hp_count(s);
        s->regs[R(offset)] = value;
        /* Starting or stopping keeps the current count */
        s->hp_held = cnt;
        s->hp_offset = cnt - imx8mm_snvs_ticks();
        break;
    case SNVS_HPSR:
        s->regs[R(offset)] &= ~(value & HPSR_BI);   /* BI is W1C */
        break;
    case SNVS_HPRTCMR:
    case SNVS_HPRTCLR:
        cnt = imx8mm_snvs_hp_count(s);
        cnt = offset == SNVS_HPRTCMR ? deposit64(cnt, 32, 15, value) :
                                       deposit64(cnt, 0, 32, value);
        s->hp_held = cnt;
        s->hp_offset = cnt - imx8mm_snvs_ticks();
        break;
    case SNVS_LPCR:
        cnt = imx8mm_snvs_lp_count(s);
        s->regs[R(offset)] = value;
        s->lp_held = cnt;
        s->lp_offset = cnt - imx8mm_snvs_ticks();
        if ((value & (LPCR_DP_EN | LPCR_TOP)) == (LPCR_DP_EN | LPCR_TOP)) {
            /* Turn off the system power (PMIC_ON_REQ deasserted) */
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        imx8mm_snvs_check_alarm(s);
        break;
    case SNVS_LPSR:
        s->regs[R(offset)] &= ~(value & LPSR_W1C_MASK);
        imx8mm_snvs_check_alarm(s);
        break;
    case SNVS_LPSRTCMR:
    case SNVS_LPSRTCLR:
        cnt = imx8mm_snvs_lp_count(s);
        cnt = offset == SNVS_LPSRTCMR ? deposit64(cnt, 32, 15, value) :
                                        deposit64(cnt, 0, 32, value);
        s->lp_held = cnt;
        s->lp_offset = cnt - imx8mm_snvs_ticks();
        imx8mm_snvs_check_alarm(s);
        break;
    case SNVS_LPTAR:
        s->regs[R(offset)] = value;
        imx8mm_snvs_check_alarm(s);
        break;
    default:
        if (offset <= SNVS_LPGPR3) {
            s->regs[R(offset)] = value;
            break;
        }
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_SNVS, offset);
        break;
    }
}

static const MemoryRegionOps imx8mm_snvs_ops = {
    .read = imx8mm_snvs_read,
    .write = imx8mm_snvs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_snvs_get_button(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    IMX8MMSNVSState *s = IMX8MM_SNVS(obj);

    visit_type_bool(v, name, &s->button, errp);
}

static void imx8mm_snvs_set_button_prop(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    bool pressed;

    if (visit_type_bool(v, name, &pressed, errp)) {
        imx8mm_snvs_set_button(IMX8MM_SNVS(obj), pressed);
    }
}

static void imx8mm_snvs_reset(DeviceState *dev)
{
    IMX8MMSNVSState *s = IMX8MM_SNVS(dev);
    struct tm tm;
    uint64_t epoch_ticks;

    memset(s->regs, 0, sizeof(s->regs));
    timer_del(s->alarm_timer);

    /*
     * The SRTC is battery backed: it holds the wall clock at power-up
     * (seconds since 1970 << 15). Software starts it with SRTC_ENV.
     */
    qemu_get_timedate(&tm, 0);
    epoch_ticks = (uint64_t)mktimegm(&tm) * RTC_FREQ;
    s->lp_held = epoch_ticks & RTC_MASK;
    s->lp_offset = s->lp_held - imx8mm_snvs_ticks();
    s->hp_held = 0;
    s->hp_offset = 0;
    imx8mm_snvs_update_irq(s);
}

static void imx8mm_snvs_init(Object *obj)
{
    IMX8MMSNVSState *s = IMX8MM_SNVS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_snvs_ops, s,
                          TYPE_IMX8MM_SNVS, IMX8MM_SNVS_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_rtc);
    sysbus_init_irq(sbd, &s->irq_pwrkey);
    qdev_init_gpio_in_named(DEVICE(obj), imx8mm_snvs_pwrkey_in,
                            IMX8MM_SNVS_PWRKEY, 1);
    s->alarm_timer = timer_new_ns(rtc_clock, imx8mm_snvs_alarm_cb, s);

    /* Run time control of the ON/OFF button, e.g. qom-set ... true/false */
    object_property_add(obj, "button-pressed", "bool",
                        imx8mm_snvs_get_button, imx8mm_snvs_set_button_prop,
                        NULL, NULL);
}

static const VMStateDescription imx8mm_snvs_vmstate = {
    .name = TYPE_IMX8MM_SNVS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMSNVSState, IMX8MM_SNVS_NUM_REGS),
        VMSTATE_INT64(lp_offset, IMX8MMSNVSState),
        VMSTATE_UINT64(lp_held, IMX8MMSNVSState),
        VMSTATE_INT64(hp_offset, IMX8MMSNVSState),
        VMSTATE_UINT64(hp_held, IMX8MMSNVSState),
        VMSTATE_BOOL(button, IMX8MMSNVSState),
        VMSTATE_TIMER_PTR(alarm_timer, IMX8MMSNVSState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx8mm_snvs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_snvs_reset);
    dc->vmsd = &imx8mm_snvs_vmstate;
    dc->desc = "i.MX8MM Secure Non-Volatile Storage";
}

static const TypeInfo imx8mm_snvs_types[] = {
    {
        .name          = TYPE_IMX8MM_SNVS,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMSNVSState),
        .instance_init = imx8mm_snvs_init,
        .class_init    = imx8mm_snvs_class_init,
    },
};

DEFINE_TYPES(imx8mm_snvs_types)
