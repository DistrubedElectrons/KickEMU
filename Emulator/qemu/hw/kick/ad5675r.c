/*
 * Analog Devices AD5671R/AD5675R octal I2C nanoDAC+ (KICK CV/Gate outputs)
 *
 * Part identification: Kick/modules/hardware/source/kick_I2c.cpp talks to
 * /dev/i2c-3 (I2C4) address 0x0c with 3 byte frames [command|address, MSB,
 * LSB]: 0x30|ch is "write to and update DAC channel n" with the 12 bit code
 * left aligned in 16 bits, 0x90|ch is "set up readback", and a 16 byte read
 * returns the 8 input registers (2 bytes each, left aligned) with the
 * channel auto-incrementing. Eight channels, 12 bit, 7-bit address
 * 0b00011A1A0 (0x0c with A1 = A0 = 0) and a 0..5 V range (2.5 V internal
 * reference, gain 2) match the AD5671R (the 16 bit AD5675R shares the
 * command set; use resolution=16 for it).
 *
 * Reference: AD5671R/AD5675R data sheet rev. B, "Commands" table:
 *   0000 no operation
 *   0001 write to input register n (dependent on LDAC)
 *   0010 update DAC register n with input register n
 *   0011 write to and update DAC channel n
 *   0100 power down/power up DAC (2 bits per channel, DAC0 in DB1:DB0)
 *   0101 hardware LDAC mask register (DB7:DB0, 1 = ignore LDAC pin)
 *   0110 software reset (DB15:DB0 = 0x1234)
 *   0111 internal reference setup (DB0 = 1 disables the reference)
 *   1001 set up readback
 * The address nibble selects DAC channel 0..7. A read returns MSB, LSB of
 * the input register of the readback channel and moves to the next one,
 * wrapping from 7 to 0.
 *
 * Output: every change of a channel output is sent as one JSON line on the
 * optional chardev (protocol in Emulator/docs/ARCHITECTURE.md):
 *   {"ch":0,"dac":2048,"volts":2.500}
 * volts = Vref * gain * code / 2^resolution, 0 when powered down or when
 * the internal reference is off and no external one is configured. The
 * state of all channels is sent when the chardev connects.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "ad5675r.h"

OBJECT_DECLARE_SIMPLE_TYPE(AD5675RState, AD5675R)

#define AD5675R_CHANNELS        8

#define CMD_NOP                 0x0
#define CMD_WRITE_INPUT         0x1
#define CMD_UPDATE_DAC          0x2
#define CMD_WRITE_UPDATE        0x3
#define CMD_POWER               0x4
#define CMD_LDAC_MASK           0x5
#define CMD_SOFT_RESET          0x6
#define CMD_REF_SETUP           0x7
#define CMD_READBACK            0x9

#define SOFT_RESET_CODE         0x1234
#define REF_SETUP_DISABLE       BIT(0)

/* Power-down modes per channel */
#define PD_NORMAL               0x0

struct AD5675RState {
    I2CSlave parent_obj;

    /* Properties */
    CharFrontend chr;
    uint32_t resolution;
    uint32_t vref_mv;           /* internal reference */
    uint32_t vref_ext_mv;       /* external VREF pin when internal is off */
    uint32_t gain;              /* GAIN pin: 1 or 2 */
    bool ldac_level_default;

    /* Registers */
    uint16_t input[AD5675R_CHANNELS];
    uint16_t dac[AD5675R_CHANNELS];
    uint16_t power;             /* 2 bits per channel */
    uint8_t ldac_mask;
    bool ref_disabled;

    /* I2C frame state */
    uint8_t frame[3];
    uint8_t frame_pos;
    uint8_t readback_ch;
    bool readback_lsb;

    /* Pins */
    bool ldac_level;
    bool in_reset;

    /* Last value sent per channel, to report changes only */
    int32_t last_mv[AD5675R_CHANNELS];
    uint16_t last_code[AD5675R_CHANNELS];
    bool input_logged;
};

static uint32_t ad5675r_channel_mv(AD5675RState *s, int ch, double *volts)
{
    uint32_t vref = s->ref_disabled ? s->vref_ext_mv : s->vref_mv;
    double v;

    if (((s->power >> (ch * 2)) & 3) != PD_NORMAL) {
        v = 0.0;
    } else {
        v = (double)vref * s->gain * s->dac[ch] / (1u << s->resolution) / 1000;
    }
    *volts = v;
    return (uint32_t)(v * 1000 + 0.5);
}

static void ad5675r_send_channel(AD5675RState *s, int ch, bool force)
{
    double volts;
    uint32_t mv = ad5675r_channel_mv(s, ch, &volts);
    char line[80];
    int len;

    if (!force && s->last_mv[ch] == mv && s->last_code[ch] == s->dac[ch]) {
        return;
    }
    s->last_mv[ch] = mv;
    s->last_code[ch] = s->dac[ch];
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }
    len = snprintf(line, sizeof(line), "{\"ch\":%d,\"dac\":%u,\"volts\":%.3f}\n",
                   ch, s->dac[ch], volts);
    /* Blocking write: one short line per DAC update */
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)line, len);
}

static void ad5675r_send_all(AD5675RState *s, bool force)
{
    int ch;

    for (ch = 0; ch < AD5675R_CHANNELS; ch++) {
        ad5675r_send_channel(s, ch, force);
    }
}

/* LDAC low: unmasked channels follow their input register */
static void ad5675r_apply_ldac(AD5675RState *s)
{
    int ch;

    if (s->ldac_level) {
        return;
    }
    for (ch = 0; ch < AD5675R_CHANNELS; ch++) {
        if (!(s->ldac_mask & BIT(ch))) {
            s->dac[ch] = s->input[ch];
        }
    }
}

static void ad5675r_power_on_reset(AD5675RState *s)
{
    /* The R grade parts power up at zero scale with the reference on */
    memset(s->input, 0, sizeof(s->input));
    memset(s->dac, 0, sizeof(s->dac));
    s->power = 0;
    s->ldac_mask = 0;
    s->ref_disabled = false;
    s->readback_ch = 0;
    s->readback_lsb = false;
    s->frame_pos = 0;
}

static void ad5675r_command(AD5675RState *s)
{
    uint8_t cmd = s->frame[0] >> 4;
    uint8_t ch = s->frame[0] & 0x0f;
    uint16_t data = (s->frame[1] << 8) | s->frame[2];
    uint16_t code = data >> (16 - s->resolution);

    if (ch >= AD5675R_CHANNELS &&
        (cmd == CMD_WRITE_INPUT || cmd == CMD_UPDATE_DAC ||
         cmd == CMD_WRITE_UPDATE || cmd == CMD_READBACK)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid DAC address %u\n",
                      TYPE_AD5675R, ch);
        return;
    }

    switch (cmd) {
    case CMD_NOP:
        break;
    case CMD_WRITE_INPUT:
        s->input[ch] = code;
        ad5675r_apply_ldac(s);
        break;
    case CMD_UPDATE_DAC:
        s->dac[ch] = s->input[ch];
        break;
    case CMD_WRITE_UPDATE:
        s->input[ch] = code;
        s->dac[ch] = code;
        break;
    case CMD_POWER:
        s->power = data;
        break;
    case CMD_LDAC_MASK:
        s->ldac_mask = data & 0xff;
        break;
    case CMD_SOFT_RESET:
        if (data == SOFT_RESET_CODE) {
            ad5675r_power_on_reset(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: software reset needs 0x%04x, "
                          "got 0x%04x\n", TYPE_AD5675R, SOFT_RESET_CODE, data);
        }
        break;
    case CMD_REF_SETUP:
        s->ref_disabled = data & REF_SETUP_DISABLE;
        break;
    case CMD_READBACK:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: reserved command 0x%x\n",
                      TYPE_AD5675R, cmd);
        return;
    }
    /* Every command byte also points the readback at its channel */
    if (ch < AD5675R_CHANNELS) {
        s->readback_ch = ch;
        s->readback_lsb = false;
    }
    ad5675r_send_all(s, false);
}

static int ad5675r_event(I2CSlave *i2c, enum i2c_event event)
{
    AD5675RState *s = AD5675R(i2c);

    if (s->in_reset && (event == I2C_START_SEND || event == I2C_START_RECV)) {
        return 1;
    }
    switch (event) {
    case I2C_START_SEND:
        s->frame_pos = 0;
        break;
    case I2C_START_RECV:
        s->readback_lsb = false;
        break;
    case I2C_FINISH:
        if (s->frame_pos) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: incomplete %u byte frame "
                          "ignored\n", TYPE_AD5675R, s->frame_pos);
            s->frame_pos = 0;
        }
        break;
    default:
        break;
    }
    return 0;
}

static int ad5675r_send(I2CSlave *i2c, uint8_t data)
{
    AD5675RState *s = AD5675R(i2c);

    s->frame[s->frame_pos++] = data;
    if (s->frame_pos == sizeof(s->frame)) {
        s->frame_pos = 0;
        ad5675r_command(s);
    }
    return 0;
}

static uint8_t ad5675r_recv(I2CSlave *i2c)
{
    AD5675RState *s = AD5675R(i2c);
    uint16_t val = s->input[s->readback_ch] << (16 - s->resolution);

    if (!s->readback_lsb) {
        s->readback_lsb = true;
        return val >> 8;
    }
    s->readback_lsb = false;
    s->readback_ch = (s->readback_ch + 1) % AD5675R_CHANNELS;
    return val & 0xff;
}

static void ad5675r_ldac(void *opaque, int n, int level)
{
    AD5675RState *s = opaque;

    s->ldac_level = level;
    ad5675r_apply_ldac(s);
    ad5675r_send_all(s, false);
}

static void ad5675r_reset_pin(void *opaque, int n, int level)
{
    AD5675RState *s = opaque;

    s->in_reset = !level;
    if (s->in_reset) {
        ad5675r_power_on_reset(s);
        ad5675r_send_all(s, false);
    }
}

static int ad5675r_chr_can_read(void *opaque)
{
    return 256;
}

static void ad5675r_chr_read(void *opaque, const uint8_t *buf, int size)
{
    AD5675RState *s = opaque;

    /* The converter has no inputs; host lines are consumed and dropped */
    if (!s->input_logged) {
        qemu_log_mask(LOG_UNIMP, "%s: host input ignored, the part has "
                      "DAC outputs only\n", TYPE_AD5675R);
        s->input_logged = true;
    }
}

static void ad5675r_chr_event(void *opaque, QEMUChrEvent event)
{
    AD5675RState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        ad5675r_send_all(s, true);
    }
}

static void ad5675r_realize(DeviceState *dev, Error **errp)
{
    AD5675RState *s = AD5675R(dev);

    if (s->resolution != 12 && s->resolution != 16) {
        error_setg(errp, "%s: resolution must be 12 (AD5671R) or 16 (AD5675R)",
                   TYPE_AD5675R);
        return;
    }
    if (s->gain != 1 && s->gain != 2) {
        error_setg(errp, "%s: gain must be 1 or 2", TYPE_AD5675R);
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, ad5675r_chr_can_read, ad5675r_chr_read,
                             ad5675r_chr_event, NULL, s, NULL, true);
}

static void ad5675r_reset(DeviceState *dev)
{
    AD5675RState *s = AD5675R(dev);
    int ch;

    ad5675r_power_on_reset(s);
    s->ldac_level = s->ldac_level_default;
    s->in_reset = false;
    for (ch = 0; ch < AD5675R_CHANNELS; ch++) {
        s->last_mv[ch] = -1;
    }
    ad5675r_send_all(s, false);
}

static void ad5675r_init(Object *obj)
{
    qdev_init_gpio_in_named(DEVICE(obj), ad5675r_ldac, AD5675R_GPIO_LDAC, 1);
    qdev_init_gpio_in_named(DEVICE(obj), ad5675r_reset_pin,
                            AD5675R_GPIO_RESET, 1);
}

static const VMStateDescription vmstate_ad5675r = {
    .name = TYPE_AD5675R,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, AD5675RState),
        VMSTATE_UINT16_ARRAY(input, AD5675RState, AD5675R_CHANNELS),
        VMSTATE_UINT16_ARRAY(dac, AD5675RState, AD5675R_CHANNELS),
        VMSTATE_UINT16(power, AD5675RState),
        VMSTATE_UINT8(ldac_mask, AD5675RState),
        VMSTATE_BOOL(ref_disabled, AD5675RState),
        VMSTATE_UINT8_ARRAY(frame, AD5675RState, 3),
        VMSTATE_UINT8(frame_pos, AD5675RState),
        VMSTATE_UINT8(readback_ch, AD5675RState),
        VMSTATE_BOOL(readback_lsb, AD5675RState),
        VMSTATE_BOOL(ldac_level, AD5675RState),
        VMSTATE_BOOL(in_reset, AD5675RState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ad5675r_properties[] = {
    DEFINE_PROP_CHR("chardev", AD5675RState, chr),
    /* 12: AD5671R (KICK), 16: AD5675R */
    DEFINE_PROP_UINT32("resolution", AD5675RState, resolution, 12),
    DEFINE_PROP_UINT32("vref-mv", AD5675RState, vref_mv, 2500),
    DEFINE_PROP_UINT32("vref-ext-mv", AD5675RState, vref_ext_mv, 0),
    DEFINE_PROP_UINT32("gain", AD5675RState, gain, 2),
    /* Board level of the LDAC pin when not driven (KICK: tied low) */
    DEFINE_PROP_BOOL("ldac-level", AD5675RState, ldac_level_default, false),
};

static void ad5675r_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = ad5675r_event;
    k->recv = ad5675r_recv;
    k->send = ad5675r_send;
    dc->realize = ad5675r_realize;
    dc->desc = "AD5671R/AD5675R octal I2C DAC (CV/Gate)";
    dc->vmsd = &vmstate_ad5675r;
    device_class_set_legacy_reset(dc, ad5675r_reset);
    device_class_set_props(dc, ad5675r_properties);
}

static const TypeInfo ad5675r_info = {
    .name          = TYPE_AD5675R,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AD5675RState),
    .instance_init = ad5675r_init,
    .class_init    = ad5675r_class_init,
};

static void ad5675r_register_types(void)
{
    type_register_static(&ad5675r_info);
}

type_init(ad5675r_register_types)
