/*
 * i.MX 8M Mini DDR controller (Synopsys uMCTL2) and DDR PHY (PUB)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_DDRC_H
#define KICK_IMX8MM_DDRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_DDRC "imx8mm.ddrc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMDDRCState, IMX8MM_DDRC)

/* MMIO 0: controller (0x3d400000), MMIO 1: PHY APB (0x3c000000) */
#define IMX8MM_DDRC_MMIO_SIZE   0x400000
#define IMX8MM_DDRPHY_MMIO_SIZE 0x1000000
#define IMX8MM_DDRC_NUM_REGS    (0x4000 / 4)
#define IMX8MM_DDRPHY_MAIL_MAX  8

struct IMX8MMDDRCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem_ctl;
    MemoryRegion iomem_phy;

    uint32_t regs[IMX8MM_DDRC_NUM_REGS];
    bool dfi_init_complete;

    /* PHY register file (sparse: firmware images are loaded through it) */
    GHashTable *phy;

    /* PMU mailbox: messages from the training firmware to the host */
    uint32_t mail[IMX8MM_DDRPHY_MAIL_MAX];
    int mail_count;
    bool mail_valid;        /* message posted, not yet acknowledged */
    bool micro_running;
    unsigned trainings;     /* completed training runs, for the log */
};

#endif /* KICK_IMX8MM_DDRC_H */
