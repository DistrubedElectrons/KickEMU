/*
 * NXP PCA9534 8-bit I2C GPIO expander with interrupt output
 *
 * KICK board: I2C2 address 0x20 ("nxp,pca9534", driver gpio-pca953x), INT
 * to GPIO1_IO07 falling edge. Pins 1..3 are the gpio-keys Back/Home/Menu
 * (active low), pins 4..7 are GPIO hogs driven low (usb3_sel, enet_rst,
 * som_vsel, enet_sel).
 *
 * Reference: PCA9534 data sheet rev. 7, section 6 "Functional description":
 *   command byte 0: Input Port (read only)
 *                1: Output Port, reset 0xFF
 *                2: Polarity Inversion, reset 0x00
 *                3: Configuration (1 = input), reset 0xFF
 * The command byte selects the register for all following data bytes of
 * the transfer; there is no auto-increment. Input Port reflects the pin
 * levels (for output pins the driven value) XOR polarity inversion.
 * INT is asserted when an input pin differs from its state at the last
 * read of the Input Port and released when the pin returns to that state
 * or the Input Port is read. The pins have no internal pull-ups; the
 * level of unconnected inputs is the "input-levels" property (board
 * pull-ups, default all high).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "pca9534.h"

OBJECT_DECLARE_SIMPLE_TYPE(PCA9534State, PCA9534)

#define PCA9534_PINS        8

#define REG_INPUT           0
#define REG_OUTPUT          1
#define REG_POLARITY        2
#define REG_CONFIG          3

struct PCA9534State {
    I2CSlave parent_obj;

    qemu_irq pin_out[PCA9534_PINS];
    qemu_irq int_out;

    uint8_t output;
    uint8_t polarity;
    uint8_t config;
    uint8_t ext_levels;     /* levels applied from outside */
    uint8_t int_ref;        /* input pin levels at the last Input Port read */
    uint8_t pins_driven;    /* last levels sent on pin_out */
    bool pins_valid;

    uint8_t cmd;
    bool cmd_received;
};

static uint8_t pca9534_pins(PCA9534State *s)
{
    return (s->config & s->ext_levels) | (~s->config & s->output);
}

static void pca9534_update(PCA9534State *s)
{
    uint8_t pins = pca9534_pins(s);
    bool irq = ((pins ^ s->int_ref) & s->config) != 0;
    int i;

    for (i = 0; i < PCA9534_PINS; i++) {
        if (!s->pins_valid || ((pins ^ s->pins_driven) & BIT(i))) {
            qemu_set_irq(s->pin_out[i], !!(pins & BIT(i)));
        }
    }
    s->pins_driven = pins;
    s->pins_valid = true;
    qemu_set_irq(s->int_out, !irq);
}

static uint8_t pca9534_read(PCA9534State *s)
{
    uint8_t pins;

    switch (s->cmd) {
    case REG_INPUT:
        pins = pca9534_pins(s);
        /* Reading the Input Port clears the interrupt */
        s->int_ref = pins;
        pca9534_update(s);
        return pins ^ s->polarity;
    case REG_OUTPUT:
        return s->output;
    case REG_POLARITY:
        return s->polarity;
    case REG_CONFIG:
        return s->config;
    default:
        g_assert_not_reached();
    }
}

static void pca9534_write(PCA9534State *s, uint8_t val)
{
    switch (s->cmd) {
    case REG_INPUT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to Input Port ignored\n",
                      TYPE_PCA9534);
        return;
    case REG_OUTPUT:
        s->output = val;
        break;
    case REG_POLARITY:
        s->polarity = val;
        break;
    case REG_CONFIG:
        s->config = val;
        break;
    default:
        g_assert_not_reached();
    }
    pca9534_update(s);
}

static int pca9534_event(I2CSlave *i2c, enum i2c_event event)
{
    PCA9534State *s = PCA9534(i2c);

    if (event == I2C_START_SEND) {
        s->cmd_received = false;
    }
    return 0;
}

static int pca9534_send(I2CSlave *i2c, uint8_t data)
{
    PCA9534State *s = PCA9534(i2c);

    if (!s->cmd_received) {
        /* Only the two LSBs of the command byte are decoded */
        if (data > REG_CONFIG) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: command byte 0x%02x\n",
                          TYPE_PCA9534, data);
        }
        s->cmd = data & 3;
        s->cmd_received = true;
        return 0;
    }
    pca9534_write(s, data);
    return 0;
}

static uint8_t pca9534_recv(I2CSlave *i2c)
{
    return pca9534_read(PCA9534(i2c));
}

static void pca9534_pin_in(void *opaque, int n, int level)
{
    PCA9534State *s = opaque;

    s->ext_levels = deposit32(s->ext_levels, n, 1, !!level);
    pca9534_update(s);
}

static void pca9534_get_levels(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    PCA9534State *s = PCA9534(obj);

    visit_type_uint8(v, name, &s->ext_levels, errp);
}

static void pca9534_set_levels(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    PCA9534State *s = PCA9534(obj);
    uint8_t val;

    if (!visit_type_uint8(v, name, &val, errp)) {
        return;
    }
    s->ext_levels = val;
    if (DEVICE(obj)->realized) {
        pca9534_update(s);
    }
}

static void pca9534_reset(DeviceState *dev)
{
    PCA9534State *s = PCA9534(dev);

    s->output = 0xff;
    s->polarity = 0;
    s->config = 0xff;
    s->cmd = 0;
    s->cmd_received = false;
    s->int_ref = pca9534_pins(s);
    s->pins_valid = false;
    pca9534_update(s);
}

static void pca9534_init(Object *obj)
{
    PCA9534State *s = PCA9534(obj);

    s->ext_levels = 0xff;
    qdev_init_gpio_in(DEVICE(obj), pca9534_pin_in, PCA9534_PINS);
    qdev_init_gpio_out(DEVICE(obj), s->pin_out, PCA9534_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), &s->int_out, PCA9534_GPIO_INT, 1);
    object_property_add(obj, "input-levels", "uint8", pca9534_get_levels,
                        pca9534_set_levels, NULL, NULL);
}

static const VMStateDescription vmstate_pca9534 = {
    .name = TYPE_PCA9534,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCA9534State),
        VMSTATE_UINT8(output, PCA9534State),
        VMSTATE_UINT8(polarity, PCA9534State),
        VMSTATE_UINT8(config, PCA9534State),
        VMSTATE_UINT8(ext_levels, PCA9534State),
        VMSTATE_UINT8(int_ref, PCA9534State),
        VMSTATE_UINT8(pins_driven, PCA9534State),
        VMSTATE_BOOL(pins_valid, PCA9534State),
        VMSTATE_UINT8(cmd, PCA9534State),
        VMSTATE_BOOL(cmd_received, PCA9534State),
        VMSTATE_END_OF_LIST()
    }
};

static void pca9534_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pca9534_event;
    k->recv = pca9534_recv;
    k->send = pca9534_send;
    dc->desc = "NXP PCA9534 8-bit I2C GPIO expander";
    dc->vmsd = &vmstate_pca9534;
    device_class_set_legacy_reset(dc, pca9534_reset);
}

static const TypeInfo pca9534_info = {
    .name          = TYPE_PCA9534,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PCA9534State),
    .instance_init = pca9534_init,
    .class_init    = pca9534_class_init,
};

static void pca9534_register_types(void)
{
    type_register_static(&pca9534_info);
}

type_init(pca9534_register_types)
