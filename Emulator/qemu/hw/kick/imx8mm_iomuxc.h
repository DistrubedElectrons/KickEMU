/*
 * i.MX 8M Mini IOMUX Controller (IOMUXC) and IOMUXC_GPR
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_IOMUXC_H
#define KICK_IMX8MM_IOMUXC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_IOMUXC "imx8mm.iomuxc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMIOMUXCState, IMX8MM_IOMUXC)

#define TYPE_IMX8MM_IOMUXC_GPR "imx8mm.iomuxc-gpr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMIOMUXCGPRState, IMX8MM_IOMUXC_GPR)

#define IMX8MM_IOMUXC_MMIO_SIZE     0x10000
/* Last register is the final SELECT_INPUT at 0x540 */
#define IMX8MM_IOMUXC_NUM_REGS      ((0x540 / 4) + 1)
/* IOMUXC_GPR0..GPR22 */
#define IMX8MM_IOMUXC_GPR_NUM_REGS  23

struct IMX8MMIOMUXCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX8MM_IOMUXC_NUM_REGS];
};

struct IMX8MMIOMUXCGPRState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX8MM_IOMUXC_GPR_NUM_REGS];
};

#endif /* KICK_IMX8MM_IOMUXC_H */
