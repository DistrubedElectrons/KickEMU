/*
 * i.MX 8M Mini uSDHC: upstream imx-usdhc with the 8MM clock and busy
 * behaviour
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_USDHC_H
#define KICK_IMX8MM_USDHC_H

#include "hw/sd/sdhci.h"
#include "qom/object.h"

#define TYPE_IMX8MM_USDHC "imx8mm-usdhc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMUSDHCState, IMX8MM_USDHC)

/* The SoC maps this sysbus MMIO region (the parent's own is index 1) */
#define IMX8MM_USDHC_MMIO   0
#define IMX8MM_USDHC_SIZE   0x100

struct IMX8MMUSDHCState {
    SDHCIState parent_obj;

    MemoryRegion container;
    MemoryRegion parent_alias;
    MemoryRegion sysctl;
    MemoryRegion status;
    MemoryRegion xfer;
};

#endif /* KICK_IMX8MM_USDHC_H */
