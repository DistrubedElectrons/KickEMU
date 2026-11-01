/*
 * TI SN65DSI83/84 MIPI DSI to LVDS bridge
 *
 * The VAR-SOM-MX8M-MINI carries an SN65DSI84 at I2C1 address 0x2c
 * (compatible "ti,sn65dsi83", node sn65dsi84@2c in imx8mm-var-som.dtsi),
 * fed by the i.MX8MM DSIM over four DSI lanes and driving the LVDS panel.
 * The KICK board DT (patches/linux/001.imx8mm_dtsi.patch) keeps that chain
 * and programs 1024x600 LVDS timings at 33 MHz in the bridge node; the LVDS
 * panel itself has no control interface.
 *
 * This is a model of the bridge's I2C configuration register file (CSR) as
 * used by drivers/gpu/drm/bridge/sn65dsi83/ in varigit linux-imx 5.4:
 *
 *  0x00-0x08  device ID "DSI85" (read backwards) and revision, read-only
 *  0x09       SOFT_RESET, self clearing, reads 0 (the driver's probe check)
 *  0x0a       PLL_EN_STAT (bit 7, read-only), LVDS_CLK_RANGE, HS_CLK_SRC
 *  0x0b       DSI_CLK_DIVIDER, REFCLK_MULTIPLIER
 *  0x0d       PLL_EN
 *  0x10-0x3e  DSI and LVDS configuration and video timing, read/write
 *  0xe0-0xe6  IRQ_EN, IRQ masks, error status (write one to clear)
 *
 * The PLL locks when PLL_EN is set and its source runs: with HS_CLK_SRC = 1
 * (the setting the driver uses) that is the DSI HS clock of the linked DSIM.
 * The EN pin is a GPIO input; while it is low the chip is in reset and does
 * not acknowledge its I2C address.
 *
 * Register reset values the driver does not depend on are zero except where
 * stated; they are not verified against the TI datasheet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/kick/sn65dsi83.h"
#include "migration/vmstate.h"

#define REG_ID0             0x00
#define REG_REV             0x08
#define REG_SOFT_RESET      0x09
#define REG_CORE_PLL        0x0a
#define REG_PLL_DIV         0x0b
#define REG_PLL_EN          0x0d
#define REG_DSI_CFG         0x10
#define REG_LAST_CFG        0x3e
#define REG_IRQ_EN          0xe0
#define REG_IRQ_MASK        0xe1
#define REG_CHA_ERR         0xe5
#define REG_CHB_ERR         0xe6

#define CORE_PLL_PLL_EN_STAT    BIT(7)
#define CORE_PLL_HS_CLK_SRC     BIT(0)
#define PLL_EN_PLL_EN           BIT(0)
#define IRQ_EN_IRQ_EN           BIT(0)
#define CHA_ERR_PLL_UNLOCK      BIT(0)

/* "DSI85" stored backwards in 0x00-0x07, revision 0x01 in 0x08. */
static const uint8_t sn65_id[] = {
    0x35, 0x38, 0x49, 0x53, 0x44, 0x20, 0x20, 0x20, 0x01
};

/* DSI_CFG reset: CHA_DSI_LANES = 4 lanes (00), SOT error tolerant. */
#define DSI_CFG_RESET       0x26

static bool sn65_pll_locked(SN65DSI83State *s)
{
    if (!s->enabled || !(s->regs[REG_PLL_EN] & PLL_EN_PLL_EN)) {
        return false;
    }
    if ((s->regs[REG_CORE_PLL] & CORE_PLL_HS_CLK_SRC) && s->dsi_host) {
        return imx8mm_dsim_hs_clock_running(s->dsi_host);
    }
    /* REFCLK input, or no DSI host linked: the reference is assumed present. */
    return true;
}

bool sn65dsi83_lvds_active(SN65DSI83State *s)
{
    return sn65_pll_locked(s) &&
           (!s->dsi_host || imx8mm_dsim_video_active(s->dsi_host));
}

static void sn65_reset_regs(SN65DSI83State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memcpy(&s->regs[REG_ID0], sn65_id, sizeof(sn65_id));
    s->regs[REG_DSI_CFG] = DSI_CFG_RESET;
    s->pointer = 0;
    s->pointer_set = false;
}

static uint8_t sn65_read_reg(SN65DSI83State *s, uint8_t reg)
{
    switch (reg) {
    case REG_SOFT_RESET:
        return 0;
    case REG_CORE_PLL:
        return (s->regs[reg] & ~CORE_PLL_PLL_EN_STAT) |
               (sn65_pll_locked(s) ? CORE_PLL_PLL_EN_STAT : 0);
    case REG_CHA_ERR:
        /* PLL_UNLOCK stays set while the enabled PLL is not locked. */
        if ((s->regs[REG_PLL_EN] & PLL_EN_PLL_EN) && !sn65_pll_locked(s)) {
            s->regs[reg] |= CHA_ERR_PLL_UNLOCK;
        }
        return s->regs[reg];
    default:
        if (reg > REG_LAST_CFG && (reg < REG_IRQ_EN || reg > REG_CHB_ERR)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: read of reserved register "
                          "0x%02x\n", __func__, reg);
        }
        return s->regs[reg];
    }
}

static void sn65_write_reg(SN65DSI83State *s, uint8_t reg, uint8_t v)
{
    if (reg <= REG_REV) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to ID register 0x%02x\n",
                      __func__, reg);
        return;
    }
    switch (reg) {
    case REG_SOFT_RESET:
        /* Resets the DSI/LVDS datapath only, the CSRs keep their values. */
        break;
    case REG_CORE_PLL:
        s->regs[reg] = v & ~CORE_PLL_PLL_EN_STAT;
        break;
    case REG_CHA_ERR:
    case REG_CHB_ERR:
        s->regs[reg] &= ~v;
        break;
    default:
        if (reg > REG_LAST_CFG && (reg < REG_IRQ_EN || reg > REG_CHB_ERR)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write 0x%02x to reserved "
                          "register 0x%02x\n", __func__, v, reg);
        }
        s->regs[reg] = v;
        break;
    }
}

static int sn65_event(I2CSlave *i2c, enum i2c_event event)
{
    SN65DSI83State *s = SN65DSI83(i2c);

    if (!s->enabled) {
        /* In reset: the address is not acknowledged. */
        return -1;
    }
    if (event == I2C_START_SEND) {
        s->pointer_set = false;
    }
    return 0;
}

static uint8_t sn65_recv(I2CSlave *i2c)
{
    SN65DSI83State *s = SN65DSI83(i2c);

    return sn65_read_reg(s, s->pointer++);
}

static int sn65_send(I2CSlave *i2c, uint8_t data)
{
    SN65DSI83State *s = SN65DSI83(i2c);

    if (!s->enabled) {
        return -1;
    }
    if (!s->pointer_set) {
        s->pointer = data;
        s->pointer_set = true;
    } else {
        sn65_write_reg(s, s->pointer++, data);
    }
    return 0;
}

static void sn65_set_enable(void *opaque, int n, int level)
{
    SN65DSI83State *s = opaque;

    if (!level && s->enabled) {
        sn65_reset_regs(s);
    }
    s->enabled = level;
}

static void sn65_reset_hold(Object *obj, ResetType type)
{
    SN65DSI83State *s = SN65DSI83(obj);

    sn65_reset_regs(s);
    s->enabled = s->en_at_reset;
}

static void sn65_init(Object *obj)
{
    qdev_init_gpio_in_named(DEVICE(obj), sn65_set_enable, "enable", 1);
}

static const Property sn65_properties[] = {
    /*
     * EN pin level until something drives the "enable" GPIO. Boards that
     * wire EN to a SoC GPIO (GPIO2_IO11 on the VAR-SOM) should set it false.
     */
    DEFINE_PROP_BOOL("en-at-reset", SN65DSI83State, en_at_reset, false),
    DEFINE_PROP_LINK("dsi-host", SN65DSI83State, dsi_host,
                     TYPE_IMX8MM_MIPI_DSIM, IMX8MMMipiDsimState *),
};

static const VMStateDescription vmstate_sn65 = {
    .name = TYPE_SN65DSI83,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, SN65DSI83State),
        VMSTATE_UINT8_ARRAY(regs, SN65DSI83State, SN65DSI83_NUM_REGS),
        VMSTATE_UINT8(pointer, SN65DSI83State),
        VMSTATE_BOOL(pointer_set, SN65DSI83State),
        VMSTATE_BOOL(enabled, SN65DSI83State),
        VMSTATE_END_OF_LIST()
    },
};

static void sn65_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "TI SN65DSI83/84 DSI to LVDS bridge";
    dc->vmsd = &vmstate_sn65;
    device_class_set_props(dc, sn65_properties);
    rc->phases.hold = sn65_reset_hold;
    k->event = sn65_event;
    k->recv = sn65_recv;
    k->send = sn65_send;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo sn65_info = {
    .name = TYPE_SN65DSI83,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(SN65DSI83State),
    .instance_init = sn65_init,
    .class_init = sn65_class_init,
};

static void sn65_register_types(void)
{
    type_register_static(&sn65_info);
}

type_init(sn65_register_types)
