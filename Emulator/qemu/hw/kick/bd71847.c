/*
 * ROHM BD71847 power management IC (I2C)
 *
 * The VAR-SOM-MX8M-MINI powers the i.MX8MM from a BD71847 on I2C1 at 0x4b
 * (imx8mm-var-som.dtsi). U-Boot SPL unlocks the regulator registers and
 * sets the DRAM and SoC rails, Linux binds the bd718x7 regulator and MFD
 * drivers to it. Register map from the BD71847AMWV datasheet and
 * include/linux/mfd/rohm-bd718x7.h:
 *
 *   0x00 REV            0x01 SWRESET        0x02 I2C_DEV
 *   0x03 PWRCTRL0       0x04 PWRCTRL1       0x05/0x06 BUCK1/2_CTRL
 *   0x09..0x0c BUCK3..6_CTRL (non-DVS)      0x0d..0x0f BUCK1 RUN/IDLE/SUSP
 *   0x10/0x11 BUCK2 RUN/IDLE                0x14..0x17 BUCK3..6 voltage
 *   0x18..0x1d LDO1..6 voltage + enable     0x1f..0x29 sequencing, faults
 *   0x2a MIRQ (mask)    0x2b IRQ (W1C)      0x2c IN_MON
 *   0x2d POW_STATE      0x2e OUT32K         0x2f REGLOCK
 *   0xff OTPVER
 *
 * The register pointer auto-increments. REGLOCK.VREG (bit 4) blocks
 * writes to the regulator registers 0x05..0x1e and REGLOCK.PWRSEQ (bit 0)
 * the sequencing registers 0x1f..0x29, as on the part. A write of 1 to
 * SWRESET.RESET restarts the system (the PMIC power cycles the SoC).
 * IRQ is the active-low INTB output (GPIO2_IO08 on the SOM), asserted
 * while any unmasked IRQ status bit is set.
 *
 * Output voltages are register state only; no other model consumes them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "system/runstate.h"

#include "bd71847.h"

#define REG_REV         0x00
#define REG_SWRESET     0x01
#define REG_VREG_FIRST  0x05
#define REG_VREG_LAST   0x1e
#define REG_SEQ_FIRST   0x1f
#define REG_SEQ_LAST    0x29
#define REG_MIRQ        0x2a
#define REG_IRQ         0x2b
#define REG_IN_MON      0x2c
#define REG_POW_STATE   0x2d
#define REG_REGLOCK     0x2f
#define REG_OTPVER      0xff

#define REGLOCK_PWRSEQ  0x01
#define REGLOCK_VREG    0x10
#define SWRESET_RESET   0x01

/* POW_STATE: the PMIC is in RUN when software can talk to it */
#define POW_STATE_RUN   0x08

struct BD71847State {
    I2CSlave parent_obj;

    uint8_t regs[256];
    uint8_t pointer;
    bool pointer_set;       /* first byte of a write sets the pointer */
    qemu_irq irq;

    uint8_t rev;
    uint8_t otpver;
};

OBJECT_DECLARE_SIMPLE_TYPE(BD71847State, BD71847)

/*
 * Reset values. Voltage codes are the power-on defaults within the
 * constraints of imx8mm-var-som.dtsi:
 *   BUCK1 (VDD_SOC) 0x14 = 0.90 V, BUCK2 (VDD_ARM) 0x1e = 1.00 V
 *   (DVS bucks: 0.70 V + code * 10 mV)
 *   BUCK3..6, LDO1..6 enabled with their OTP default code.
 */
static const uint8_t bd71847_reset_regs[][2] = {
    { 0x02, 0x4b },     /* I2C_DEV: own address */
    { 0x05, 0x44 },     /* BUCK1_CTRL: RUN_ON, ramp 10 mV/us */
    { 0x06, 0x44 },     /* BUCK2_CTRL */
    { 0x09, 0x04 },     /* BUCK3_CTRL (BUCK5 in the datasheet) */
    { 0x0a, 0x04 },     /* BUCK4_CTRL (BUCK6) */
    { 0x0b, 0x04 },     /* BUCK5_CTRL (BUCK7) */
    { 0x0c, 0x04 },     /* BUCK6_CTRL (BUCK8) */
    { 0x0d, 0x14 },     /* BUCK1_VOLT_RUN 0.90 V */
    { 0x0e, 0x14 },     /* BUCK1_VOLT_IDLE */
    { 0x0f, 0x14 },     /* BUCK1_VOLT_SUSP */
    { 0x10, 0x1e },     /* BUCK2_VOLT_RUN 1.00 V */
    { 0x11, 0x14 },     /* BUCK2_VOLT_IDLE 0.90 V */
    { 0x14, 0x00 },     /* BUCK3_VOLT */
    { 0x15, 0x03 },     /* BUCK4_VOLT */
    { 0x16, 0x03 },     /* BUCK5_VOLT */
    { 0x17, 0x1e },     /* BUCK6_VOLT */
    { 0x18, 0x40 },     /* LDO1: enabled */
    { 0x19, 0x40 },     /* LDO2 */
    { 0x1a, 0x40 },     /* LDO3 */
    { 0x1b, 0x40 },     /* LDO4 */
    { 0x1c, 0x40 },     /* LDO5 */
    { 0x1d, 0x40 },     /* LDO6 */
    { 0x2a, 0xff },     /* MIRQ: all interrupts masked */
    { 0x2d, POW_STATE_RUN },
    { 0x2e, 0x01 },     /* OUT32K enabled */
    { 0x2f, REGLOCK_PWRSEQ | REGLOCK_VREG },
};

static void bd71847_update_irq(BD71847State *s)
{
    bool pending = s->regs[REG_IRQ] & ~s->regs[REG_MIRQ];

    /* INTB is active low */
    qemu_set_irq(s->irq, !pending);
}

static bool bd71847_writable(BD71847State *s, uint8_t reg)
{
    switch (reg) {
    case REG_REV:
    case REG_IN_MON:
    case REG_POW_STATE:
    case REG_OTPVER:
        return false;
    case REG_VREG_FIRST ... REG_VREG_LAST:
        return !(s->regs[REG_REGLOCK] & REGLOCK_VREG);
    case REG_SEQ_FIRST ... REG_SEQ_LAST:
        return !(s->regs[REG_REGLOCK] & REGLOCK_PWRSEQ);
    default:
        return reg <= REG_REGLOCK;
    }
}

static void bd71847_write_reg(BD71847State *s, uint8_t reg, uint8_t value)
{
    if (!bd71847_writable(s, reg)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write 0x%02x to %s register "
                      "0x%02x ignored\n", TYPE_BD71847, value,
                      reg <= REG_REGLOCK ? "locked" : "read-only", reg);
        return;
    }

    switch (reg) {
    case REG_SWRESET:
        s->regs[reg] = value & ~SWRESET_RESET;
        if (value & SWRESET_RESET) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        break;
    case REG_IRQ:
        s->regs[reg] &= ~value;     /* W1C */
        bd71847_update_irq(s);
        break;
    case REG_MIRQ:
        s->regs[reg] = value;
        bd71847_update_irq(s);
        break;
    default:
        s->regs[reg] = value;
        break;
    }
}

static int bd71847_event(I2CSlave *i2c, enum i2c_event event)
{
    BD71847State *s = BD71847(i2c);

    if (event == I2C_START_SEND) {
        s->pointer_set = false;
    }
    return 0;
}

static uint8_t bd71847_recv(I2CSlave *i2c)
{
    BD71847State *s = BD71847(i2c);

    return s->regs[s->pointer++];
}

static int bd71847_send(I2CSlave *i2c, uint8_t data)
{
    BD71847State *s = BD71847(i2c);

    if (!s->pointer_set) {
        s->pointer = data;
        s->pointer_set = true;
        return 0;
    }
    bd71847_write_reg(s, s->pointer++, data);
    return 0;
}

static void bd71847_reset(DeviceState *dev)
{
    BD71847State *s = BD71847(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (int i = 0; i < ARRAY_SIZE(bd71847_reset_regs); i++) {
        s->regs[bd71847_reset_regs[i][0]] = bd71847_reset_regs[i][1];
    }
    s->regs[REG_REV] = s->rev;
    s->regs[REG_OTPVER] = s->otpver;
    s->pointer = 0;
    s->pointer_set = false;
    bd71847_update_irq(s);
}

static void bd71847_init(Object *obj)
{
    BD71847State *s = BD71847(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, BD71847_GPIO_IRQ, 1);
}

static const VMStateDescription bd71847_vmstate = {
    .name = TYPE_BD71847,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, BD71847State),
        VMSTATE_UINT8_ARRAY(regs, BD71847State, 256),
        VMSTATE_UINT8(pointer, BD71847State),
        VMSTATE_BOOL(pointer_set, BD71847State),
        VMSTATE_END_OF_LIST()
    },
};

static const Property bd71847_properties[] = {
    /* Product revision and OTP version as read from the part */
    DEFINE_PROP_UINT8("rev", BD71847State, rev, 0xa0),
    DEFINE_PROP_UINT8("otpver", BD71847State, otpver, 0x00),
};

static void bd71847_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    device_class_set_legacy_reset(dc, bd71847_reset);
    device_class_set_props(dc, bd71847_properties);
    dc->vmsd = &bd71847_vmstate;
    dc->desc = "ROHM BD71847 PMIC";
    k->event = bd71847_event;
    k->recv = bd71847_recv;
    k->send = bd71847_send;
}

static const TypeInfo bd71847_types[] = {
    {
        .name          = TYPE_BD71847,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(BD71847State),
        .instance_init = bd71847_init,
        .class_init    = bd71847_class_init,
    },
};

DEFINE_TYPES(bd71847_types)
