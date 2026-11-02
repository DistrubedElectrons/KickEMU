/*
 * i.MX 8M Mini Secure Non-Volatile Storage (SNVS_HP / SNVS_LP)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_SNVS_H
#define KICK_IMX8MM_SNVS_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_IMX8MM_SNVS "imx8mm.snvs"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMSNVSState, IMX8MM_SNVS)

#define IMX8MM_SNVS_MMIO_SIZE   0x10000
#define IMX8MM_SNVS_NUM_REGS    (0xa0 / 4)

struct IMX8MMSNVSState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq_rtc;       /* SNVS consolidated (RTC alarm), SPI 19 */
    qemu_irq irq_pwrkey;    /* ON/OFF button, SPI 4 */
    QEMUTimer *alarm_timer;
    QEMUTimer *hp_alarm_timer;
    QEMUTimer *press_timer;     /* long-press power off */

    uint32_t regs[IMX8MM_SNVS_NUM_REGS];

    /*
     * Both real time counters count 32.768 kHz ticks. A running counter is
     * rtc_clock based: value = offset + ticks(now); a stopped one holds
     * its value in *_held.
     */
    int64_t lp_offset;
    uint64_t lp_held;
    int64_t hp_offset;
    uint64_t hp_held;

    bool button;            /* ON/OFF button level, true = pressed */
};

/* GPIO input name for the ON/OFF button (level, 1 = pressed) */
#define IMX8MM_SNVS_PWRKEY "power-key"

#endif /* KICK_IMX8MM_SNVS_H */
