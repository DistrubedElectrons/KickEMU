/*
 * TI SN65DSI83/84 MIPI DSI to LVDS bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_SN65DSI83_H
#define KICK_SN65DSI83_H

#include "hw/i2c/i2c.h"
#include "hw/kick/imx8mm_mipi_dsim.h"
#include "qom/object.h"

#define TYPE_SN65DSI83 "sn65dsi83"
OBJECT_DECLARE_SIMPLE_TYPE(SN65DSI83State, SN65DSI83)

#define SN65DSI83_NUM_REGS  256

struct SN65DSI83State {
    I2CSlave parent_obj;

    uint8_t regs[SN65DSI83_NUM_REGS];
    uint8_t pointer;
    bool pointer_set;           /* first byte of a write sets the pointer */
    bool enabled;               /* EN pin level */
    IMX8MMMipiDsimState *dsi_host;

    /* Properties. */
    bool en_at_reset;
};

/* True once the bridge drives the LVDS output (PLL locked, out of reset). */
bool sn65dsi83_lvds_active(SN65DSI83State *s);

#endif
