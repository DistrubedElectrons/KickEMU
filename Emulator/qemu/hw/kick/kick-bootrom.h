/*
 * i.MX 8M Mini BootROM emulation (boot=rom)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_BOOTROM_H
#define KICK_BOOTROM_H

#include "fsl-imx8mm.h"
#include "system/block-backend.h"

/* Boot device as recorded in the ROM's boot info (U-Boot boot_mode.h) */
typedef enum KickBootDevType {
    KICK_BOOTDEV_SD  = 1,   /* BOOT_TYPE_SD */
    KICK_BOOTDEV_MMC = 2,   /* BOOT_TYPE_MMC */
} KickBootDevType;

/*
 * Read the boot image from @blk the way the i.MX8MM BootROM does (IVT at
 * 33 KiB on SD/eMMC), place it in on-chip memory, publish the boot device
 * for SPL and point CPU 0 at the image entry. @instance is the uSDHC index
 * (0-based) the device is attached to.
 */
bool kick_bootrom_setup(FslImx8mmState *soc, BlockBackend *blk,
                        KickBootDevType type, int instance, Error **errp);

#endif /* KICK_BOOTROM_H */
