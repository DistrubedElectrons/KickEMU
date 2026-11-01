/*
 * i.MX 8M Mini uSDHC command queue engine (CQHCI) register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_CQHCI_H
#define KICK_IMX8MM_CQHCI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_CQHCI "imx8mm.usdhc-cqhci"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMCQHCIState, IMX8MM_CQHCI)

/* Located at uSDHC base + 0x100 */
#define IMX8MM_CQHCI_OFFSET     0x100
#define IMX8MM_CQHCI_MMIO_SIZE  0x100
#define IMX8MM_CQHCI_NUM_REGS   (0x60 / 4)

struct IMX8MMCQHCIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX8MM_CQHCI_NUM_REGS];

    /* Properties */
    uint32_t version;
    uint32_t capabilities;
};

#endif /* KICK_IMX8MM_CQHCI_H */
