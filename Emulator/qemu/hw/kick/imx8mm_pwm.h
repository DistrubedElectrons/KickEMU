/*
 * i.MX 8M Mini PWM (the i.MX27-style PWMv2 block)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_PWM_H
#define KICK_IMX8MM_PWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_PWM "imx8mm.pwm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMPWMState, IMX8MM_PWM)

#define IMX8MM_PWM_MMIO_SIZE    0x10000

struct IMX8MMPWMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t cr;        /* PWMCR */
    uint32_t sr;        /* PWMSR: only the sticky event bits are stored */
    uint32_t ir;        /* PWMIR */
    uint32_t sar;       /* active sample (the FIFO drains at once) */
    uint32_t pr;        /* PWMPR */
};

/* Output duty in 1/1000 of the period, 0 when the PWM is disabled or the
 * period is not set. Inverted polarity (POUTC = 1) is taken into account. */
uint32_t imx8mm_pwm_duty_permille(IMX8MMPWMState *s);

#endif /* KICK_IMX8MM_PWM_H */
