/*
 * Qualcomm Atheros AR8033 Gigabit Ethernet PHY on the i.MX ENET MDIO bus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_AR8033_PHY_H
#define KICK_AR8033_PHY_H

#include "hw/core/qdev.h"
#include "hw/net/imx_fec.h"
#include "qom/object.h"

#define TYPE_AR8033_PHY "ar8033-phy"
OBJECT_DECLARE_SIMPLE_TYPE(AR8033PhyState, AR8033_PHY)

struct AR8033PhyState {
    DeviceState parent_obj;

    /* MMFR window laid over the ENET register block */
    MemoryRegion mmfr;
    uint32_t mmfr_value;

    uint16_t regs[32];
    uint16_t debug[64];
    uint16_t debug_addr;
    uint16_t mmd_ctrl;
    uint16_t mmd_addr;
    GHashTable *mmd;
    bool link_up;

    /* Properties */
    IMXFECState *enet;
    uint64_t enet_base;
    uint32_t phy_addr;
};

#endif /* KICK_AR8033_PHY_H */
