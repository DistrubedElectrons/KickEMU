/*
 * i.MX 8M Mini I2C: upstream imx_i2c with the byte-complete flag on NACK
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_I2C_H
#define KICK_IMX8MM_I2C_H

#include "hw/i2c/imx_i2c.h"
#include "qom/object.h"

#define TYPE_IMX8MM_I2C "imx8mm-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMI2CState, IMX8MM_I2C)

/* The SoC maps this sysbus MMIO region (the parent's own is index 1) */
#define IMX8MM_I2C_MMIO     0

struct IMX8MMI2CState {
    IMXI2CState parent_obj;

    MemoryRegion container;
    MemoryRegion parent_alias;
    MemoryRegion i2dr;
};

#endif /* KICK_IMX8MM_I2C_H */
