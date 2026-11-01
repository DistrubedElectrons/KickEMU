/*
 * i.MX 8M Mini System Reset Controller (SRC)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_SRC_H
#define KICK_IMX8MM_SRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_SRC "imx8mm.src"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMSRCState, IMX8MM_SRC)

#define IMX8MM_SRC_MMIO_SIZE    0x10000
/* Registers 0x000..0x0fc and the DDR controller reset block at 0x1000 */
#define IMX8MM_SRC_NUM_REGS     (0x100 / 4)
#define IMX8MM_SRC_NUM_DDR_REGS 2

struct IMX8MMSRCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX8MM_SRC_NUM_REGS];
    uint32_t ddr_regs[IMX8MM_SRC_NUM_DDR_REGS];

    /* Properties */
    uint32_t sbmr1;
    uint32_t sbmr2;
    uint32_t num_cpus;
};

#endif /* KICK_IMX8MM_SRC_H */
