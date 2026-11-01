/*
 * i.MX 8M Mini Thermal Monitoring Unit (TMU)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_TMU_H
#define KICK_IMX8MM_TMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_TMU "imx8mm.tmu"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMTMUState, IMX8MM_TMU)

#define IMX8MM_TMU_MMIO_SIZE    0x10000
/* Registers 0x00..0x2c plus the calibration/test space up to 0xfff */
#define IMX8MM_TMU_NUM_REGS     (0x1000 / 4)

struct IMX8MMTMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX8MM_TMU_NUM_REGS];

    /* Die temperature in degrees Celsius, settable at run time (QOM) */
    int32_t temperature;
};

#endif /* KICK_IMX8MM_TMU_H */
