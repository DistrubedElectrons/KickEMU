/*
 * i.MX 8M Mini General Power Controller (GPC)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_GPC_H
#define KICK_IMX8MM_GPC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_GPC "imx8mm.gpc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMGPCState, IMX8MM_GPC)

#define IMX8MM_GPC_MMIO_SIZE    0x10000
#define IMX8MM_GPC_NUM_REGS     (0x1000 / 4)

struct IMX8MMGPCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX8MM_GPC_NUM_REGS];
};

#endif /* KICK_IMX8MM_GPC_H */
