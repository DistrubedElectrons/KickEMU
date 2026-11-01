/*
 * Goodix GT911 capacitive touch controller (I2C slave)
 *
 * Register level model of the GT911 as used on the KICK 800x480 panel
 * (DT: goodix,gt911 at 0x14 on I2C3, INT on GPIO5_IO4, RSTB on GPIO5_IO3).
 *
 * References:
 *  - Goodix GT911 Programming Guide rev. 09 (register map, point format,
 *    configuration block, INT trigger modes, address selection timing)
 *  - Linux drivers/input/touchscreen/goodix.c (varigit linux-imx 5.4 with
 *    the KICK patch 003.goodix.c.patch), which reads 0x8140 (ID/version),
 *    the 186 byte config block at 0x8047, polls 0x814E and clears it by
 *    writing 0.
 *
 * Protocol: every transfer starts with a 16 bit big-endian register address
 * written by the host. Further written bytes are stored with auto-increment,
 * reads return data from the current address with auto-increment.
 *
 * Input: the device registers a QEMU input handler for multitouch (MTT) and
 * absolute pointer (ABS + BTN) events, so QMP input-send-event and the UI
 * frontends drive it. QEMU absolute axes span 0..0x7fff and are scaled to
 * the configured X/Y output maximum.
 *
 * Reporting follows the chip: while fingers are down, a report is produced
 * every scan period (5 + Refresh_Rate[3:0] ms from the config block). A new
 * report is only published when the host has cleared the buffer status bit
 * of 0x814E. Each published report generates an INT event according to
 * Module_Switch1[1:0]: an edge pulse of int-pulse-us for the edge modes, a
 * level held until the host clears 0x814E for the level modes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "ui/input.h"
#include "goodix_gt911.h"

OBJECT_DECLARE_SIMPLE_TYPE(GT911State, GOODIX_GT911)

/* Register map (programming guide section "Register Table") */
#define GT911_REG_COMMAND       0x8040  /* real-time command, write only */
#define GT911_REG_ESD_CHECK     0x8041  /* host writes 0xAA, firmware clears */
#define GT911_REG_CMD_CHECK     0x8046  /* command check, written before 0x8040 */
#define GT911_REG_CONFIG        0x8047  /* config block start (Config_Version) */
#define GT911_CONFIG_LEN        186     /* 0x8047..0x8100 incl. checksum+fresh */
#define GT911_CONFIG_CHKSUM     184     /* offset of Config_Chksum (0x80FF) */
#define GT911_CONFIG_FRESH      185     /* offset of Config_Fresh (0x8100) */
#define GT911_REG_PRODUCT_ID    0x8140  /* 4 ASCII bytes, "911\0" */
#define GT911_REG_FW_VERSION    0x8144  /* little endian */
#define GT911_REG_X_RES         0x8146  /* little endian */
#define GT911_REG_Y_RES         0x8148  /* little endian */
#define GT911_REG_VENDOR_ID     0x814A
#define GT911_REG_STATUS        0x814E  /* buffer status / number of points */
#define GT911_REG_POINT1        0x814F  /* 8 bytes per point */
#define GT911_POINT_SIZE        8
#define GT911_MAX_POINTS        5
#define GT911_REG_POINTS_END    (GT911_REG_POINT1 + \
                                 GT911_MAX_POINTS * GT911_POINT_SIZE)

/* 0x814E bits */
#define GT911_STATUS_BUFFER     BIT(7)
#define GT911_STATUS_NPOINTS    0x0f

/* Config block field offsets (relative to 0x8047) */
#define CFG_VERSION             0
#define CFG_X_MAX               1
#define CFG_Y_MAX               3
#define CFG_TOUCH_NUMBER        5
#define CFG_MODULE_SWITCH1      6
#define CFG_SHAKE_COUNT         8
#define CFG_FILTER              9
#define CFG_LARGE_TOUCH         10
#define CFG_NOISE_REDUCTION     11
#define CFG_TOUCH_LEVEL         12
#define CFG_LEAVE_LEVEL         13
#define CFG_LOW_POWER           14
#define CFG_REFRESH_RATE        15
#define CFG_SENSOR_CH0          (0x80B7 - GT911_REG_CONFIG)
#define CFG_SENSOR_CH_NUM       14
#define CFG_DRIVER_CH0          (0x80D5 - GT911_REG_CONFIG)
#define CFG_DRIVER_CH_NUM       26

#define MODULE_SWITCH1_INT_MASK 0x03
#define MODULE_SWITCH1_X2Y      BIT(3)

/* Module_Switch1[1:0] interrupt trigger modes */
enum {
    GT911_INT_RISING = 0,
    GT911_INT_FALLING = 1,
    GT911_INT_LOW = 2,
    GT911_INT_HIGH = 3,
};

/* 7-bit addresses selected by the INT level at the end of reset */
#define GT911_ADDR_INT_HIGH     0x14
#define GT911_ADDR_INT_LOW      0x5d

/* Real-time commands written to 0x8040 */
#define GT911_CMD_READ_COORD    0x00
#define GT911_CMD_SCREEN_OFF    0x05

typedef struct GT911Contact {
    bool down;
    uint16_t x;     /* QEMU absolute axis value, 0..0x7fff */
    uint16_t y;
} GT911Contact;

struct GT911State {
    I2CSlave parent_obj;

    /* Properties */
    uint32_t width;
    uint32_t height;
    uint32_t max_contacts;
    uint32_t int_trigger;
    uint32_t touch_size;
    uint32_t fw_version;
    uint32_t vendor_id;
    uint32_t int_pulse_us;

    qemu_irq int_out;
    QemuInputHandlerState *input;
    QEMUTimer *scan_timer;
    QEMUTimer *pulse_timer;

    /* I2C transfer state */
    uint16_t addr;
    uint8_t addr_bytes;     /* number of address bytes received */

    /* Register contents */
    uint8_t config[GT911_CONFIG_LEN];
    uint8_t command;
    uint8_t cmd_check;
    uint8_t esd_check;
    uint8_t status;
    uint8_t points[GT911_MAX_POINTS * GT911_POINT_SIZE];

    /* Input state: contacts indexed by QEMU MTT slot, slot == track ID */
    GT911Contact contacts[GT911_MAX_POINTS];
    uint32_t cur_slot;
    bool mtt_seen;          /* MTT events received, ignore pointer emulation */
    GT911Contact pointer;   /* single contact driven by ABS + BTN_LEFT */
    bool dirty;             /* input changed since the last published report */
    bool reported_down;     /* last published report had at least one point */

    /* Pins */
    bool in_reset;
    bool int_sense;
    bool int_active;
    bool screen_off;
};

static uint16_t gt911_cfg16(GT911State *s, int off)
{
    return s->config[off] | (s->config[off + 1] << 8);
}

static uint8_t gt911_trigger(GT911State *s)
{
    return s->config[CFG_MODULE_SWITCH1] & MODULE_SWITCH1_INT_MASK;
}

static uint8_t gt911_config_checksum(const uint8_t *cfg)
{
    uint8_t sum = 0;
    int i;

    /* Two's complement of the byte sum of 0x8047..0x80FE */
    for (i = 0; i < GT911_CONFIG_CHKSUM; i++) {
        sum += cfg[i];
    }
    return (uint8_t)(~sum + 1);
}

/* Drive the INT pin. "active" is the asserted state of the configured mode. */
static void gt911_set_int(GT911State *s, bool active)
{
    uint8_t trig = gt911_trigger(s);
    bool active_high = (trig == GT911_INT_RISING || trig == GT911_INT_HIGH);

    s->int_active = active;
    qemu_set_irq(s->int_out, active == active_high);
}

static void gt911_pulse_end(void *opaque)
{
    GT911State *s = opaque;

    gt911_set_int(s, false);
}

static void gt911_signal_report(GT911State *s)
{
    uint8_t trig = gt911_trigger(s);

    gt911_set_int(s, true);
    if (trig == GT911_INT_RISING || trig == GT911_INT_FALLING) {
        timer_mod(s->pulse_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)s->int_pulse_us * SCALE_US);
    }
}

static int64_t gt911_scan_period_ns(GT911State *s)
{
    /* Refresh_Rate[3:0]: report period is 5 + N ms */
    return (5 + (s->config[CFG_REFRESH_RATE] & 0x0f)) * SCALE_MS;
}

static bool gt911_any_down(GT911State *s)
{
    int i;

    if (s->mtt_seen) {
        for (i = 0; i < GT911_MAX_POINTS; i++) {
            if (s->contacts[i].down) {
                return true;
            }
        }
        return false;
    }
    return s->pointer.down;
}

/* Copy the current input state into the point registers. */
static void gt911_publish(GT911State *s)
{
    uint16_t xmax = gt911_cfg16(s, CFG_X_MAX);
    uint16_t ymax = gt911_cfg16(s, CFG_Y_MAX);
    uint8_t limit = MIN(s->config[CFG_TOUCH_NUMBER] & 0x0f, GT911_MAX_POINTS);
    bool x2y = s->config[CFG_MODULE_SWITCH1] & MODULE_SWITCH1_X2Y;
    int n = 0, i;

    memset(s->points, 0, sizeof(s->points));
    for (i = 0; i < GT911_MAX_POINTS && n < limit; i++) {
        const GT911Contact *c = s->mtt_seen ? &s->contacts[i] : &s->pointer;
        uint8_t *p = &s->points[n * GT911_POINT_SIZE];
        uint32_t x, y;

        if (!s->mtt_seen && i > 0) {
            break;
        }
        if (!c->down) {
            continue;
        }
        /* Scale 0..0x7fff to 0..max-1 */
        x = (uint32_t)c->x * xmax / (INPUT_EVENT_ABS_MAX + 1);
        y = (uint32_t)c->y * ymax / (INPUT_EVENT_ABS_MAX + 1);
        if (x2y) {
            uint32_t t = x;
            x = y;
            y = t;
        }
        p[0] = i;                       /* track ID */
        p[1] = x & 0xff;
        p[2] = x >> 8;
        p[3] = y & 0xff;
        p[4] = y >> 8;
        p[5] = s->touch_size & 0xff;    /* point size */
        p[6] = s->touch_size >> 8;
        p[7] = 0;                       /* reserved */
        n++;
    }
    s->status = GT911_STATUS_BUFFER | n;
    s->reported_down = n > 0;
    s->dirty = false;
    gt911_signal_report(s);
}

static void gt911_schedule_scan(GT911State *s)
{
    if (!timer_pending(s->scan_timer)) {
        timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  gt911_scan_period_ns(s));
    }
}

/*
 * Scan cycle: publish a report when the input changed or fingers are down,
 * but only once the host consumed the previous one.
 */
static void gt911_scan(void *opaque)
{
    GT911State *s = opaque;
    bool down = gt911_any_down(s);

    if (s->in_reset || s->screen_off) {
        return;
    }
    if (s->dirty || down || s->reported_down) {
        if (!(s->status & GT911_STATUS_BUFFER)) {
            gt911_publish(s);
        }
    }
    if (s->dirty || gt911_any_down(s) || s->reported_down) {
        gt911_schedule_scan(s);
    }
}

static void gt911_input_event(DeviceState *dev, QemuConsole *src,
                              InputEvent *evt)
{
    GT911State *s = GOODIX_GT911(dev);
    InputMultiTouchEvent *mtt;
    InputMoveEvent *move;
    InputBtnEvent *btn;
    GT911Contact *c;

    switch (evt->type) {
    case INPUT_EVENT_KIND_MTT:
        mtt = evt->u.mtt.data;
        s->mtt_seen = true;
        if (mtt->slot < 0 || mtt->slot >= GT911_MAX_POINTS) {
            /* The controller tracks at most five contacts */
            return;
        }
        c = &s->contacts[mtt->slot];
        s->cur_slot = mtt->slot;
        switch (mtt->type) {
        case INPUT_MULTI_TOUCH_TYPE_BEGIN:
        case INPUT_MULTI_TOUCH_TYPE_UPDATE:
            c->down = true;
            break;
        case INPUT_MULTI_TOUCH_TYPE_END:
        case INPUT_MULTI_TOUCH_TYPE_CANCEL:
            c->down = false;
            break;
        case INPUT_MULTI_TOUCH_TYPE_DATA:
            if (mtt->axis == INPUT_AXIS_X) {
                c->x = MIN(mtt->value, INPUT_EVENT_ABS_MAX);
            } else if (mtt->axis == INPUT_AXIS_Y) {
                c->y = MIN(mtt->value, INPUT_EVENT_ABS_MAX);
            }
            break;
        default:
            return;
        }
        s->dirty = true;
        break;
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            s->pointer.x = MIN(move->value, INPUT_EVENT_ABS_MAX);
        } else if (move->axis == INPUT_AXIS_Y) {
            s->pointer.y = MIN(move->value, INPUT_EVENT_ABS_MAX);
        }
        if (!s->mtt_seen && s->pointer.down) {
            s->dirty = true;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_LEFT || btn->button == INPUT_BUTTON_TOUCH) {
            s->pointer.down = btn->down;
            if (!s->mtt_seen) {
                s->dirty = true;
            }
        }
        break;
    default:
        break;
    }
}

static void gt911_input_sync(DeviceState *dev)
{
    GT911State *s = GOODIX_GT911(dev);

    if (s->dirty) {
        gt911_schedule_scan(s);
    }
}

static const QemuInputHandler gt911_input_handler = {
    .name  = "Goodix GT911 touchscreen",
    .mask  = INPUT_EVENT_MASK_MTT | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = gt911_input_event,
    .sync  = gt911_input_sync,
};

static uint8_t gt911_read_reg(GT911State *s, uint16_t reg)
{
    if (reg >= GT911_REG_CONFIG && reg < GT911_REG_CONFIG + GT911_CONFIG_LEN) {
        return s->config[reg - GT911_REG_CONFIG];
    }
    if (reg >= GT911_REG_POINT1 && reg < GT911_REG_POINTS_END) {
        return s->points[reg - GT911_REG_POINT1];
    }
    switch (reg) {
    case GT911_REG_COMMAND:
        return s->command;
    case GT911_REG_ESD_CHECK:
        return s->esd_check;
    case GT911_REG_CMD_CHECK:
        return s->cmd_check;
    case GT911_REG_PRODUCT_ID:
        return '9';
    case GT911_REG_PRODUCT_ID + 1:
        return '1';
    case GT911_REG_PRODUCT_ID + 2:
        return '1';
    case GT911_REG_PRODUCT_ID + 3:
        return 0;
    case GT911_REG_FW_VERSION:
        return s->fw_version & 0xff;
    case GT911_REG_FW_VERSION + 1:
        return s->fw_version >> 8;
    case GT911_REG_X_RES:
        return s->config[CFG_X_MAX];
    case GT911_REG_X_RES + 1:
        return s->config[CFG_X_MAX + 1];
    case GT911_REG_Y_RES:
        return s->config[CFG_Y_MAX];
    case GT911_REG_Y_RES + 1:
        return s->config[CFG_Y_MAX + 1];
    case GT911_REG_VENDOR_ID:
        return s->vendor_id;
    case GT911_REG_STATUS:
        return s->status;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read of unimplemented register 0x%04x\n",
                      TYPE_GOODIX_GT911, reg);
        return 0;
    }
}

static void gt911_command(GT911State *s, uint8_t cmd)
{
    s->command = cmd;
    switch (cmd) {
    case GT911_CMD_READ_COORD:
        break;
    case GT911_CMD_SCREEN_OFF:
        /* Sleep: no scanning until the host wakes the chip via INT/reset */
        s->screen_off = true;
        timer_del(s->scan_timer);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: real-time command 0x%02x not modelled\n",
                      TYPE_GOODIX_GT911, cmd);
        break;
    }
}

/*
 * The firmware takes a config written by the host when Config_Fresh is set
 * and the checksum matches; otherwise the previous config stays active.
 */
static void gt911_config_written(GT911State *s, const uint8_t *shadow)
{
    if (shadow[GT911_CONFIG_FRESH] != 1) {
        return;
    }
    if (gt911_config_checksum(shadow) != shadow[GT911_CONFIG_CHKSUM]) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: config checksum mismatch, ignored\n",
                      TYPE_GOODIX_GT911);
        return;
    }
    memcpy(s->config, shadow, GT911_CONFIG_LEN);
    gt911_set_int(s, s->int_active);
}

static void gt911_write_reg(GT911State *s, uint16_t reg, uint8_t val)
{
    if (reg >= GT911_REG_CONFIG && reg < GT911_REG_CONFIG + GT911_CONFIG_LEN) {
        uint8_t shadow[GT911_CONFIG_LEN];

        /*
         * Bytes are collected in place; the block is (re)validated whenever
         * Config_Fresh is written, which is the last byte of a config write.
         */
        memcpy(shadow, s->config, sizeof(shadow));
        shadow[reg - GT911_REG_CONFIG] = val;
        if (reg - GT911_REG_CONFIG == GT911_CONFIG_FRESH) {
            gt911_config_written(s, shadow);
        } else {
            s->config[reg - GT911_REG_CONFIG] = val;
        }
        return;
    }
    switch (reg) {
    case GT911_REG_COMMAND:
        gt911_command(s, val);
        break;
    case GT911_REG_ESD_CHECK:
        /* The firmware clears the ESD check byte on its next scan */
        s->esd_check = 0;
        break;
    case GT911_REG_CMD_CHECK:
        s->cmd_check = val;
        break;
    case GT911_REG_STATUS:
        /* Writing 0 releases the buffer for the next report */
        s->status = val;
        if (!(val & GT911_STATUS_BUFFER)) {
            uint8_t trig = gt911_trigger(s);

            if (trig == GT911_INT_LOW || trig == GT911_INT_HIGH) {
                gt911_set_int(s, false);
            }
            if (s->dirty || gt911_any_down(s) || s->reported_down) {
                gt911_schedule_scan(s);
            }
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write 0x%02x to read-only/unknown register 0x%04x\n",
                      TYPE_GOODIX_GT911, val, reg);
        break;
    }
}

static int gt911_event(I2CSlave *i2c, enum i2c_event event)
{
    GT911State *s = GOODIX_GT911(i2c);

    if (s->in_reset) {
        /* The controller does not acknowledge while RSTB is low */
        return event == I2C_START_SEND || event == I2C_START_RECV ? 1 : 0;
    }
    if (event == I2C_START_SEND) {
        s->addr_bytes = 0;
    }
    return 0;
}

static int gt911_send(I2CSlave *i2c, uint8_t data)
{
    GT911State *s = GOODIX_GT911(i2c);

    if (s->in_reset) {
        return 1;
    }
    if (s->addr_bytes < 2) {
        /* Register address, MSB first */
        s->addr = (s->addr << 8) | data;
        s->addr_bytes++;
        return 0;
    }
    gt911_write_reg(s, s->addr, data);
    s->addr++;
    return 0;
}

static uint8_t gt911_recv(I2CSlave *i2c)
{
    GT911State *s = GOODIX_GT911(i2c);
    uint8_t val;

    if (s->in_reset) {
        return 0xff;
    }
    val = gt911_read_reg(s, s->addr);
    s->addr++;
    return val;
}

static void gt911_build_config(GT911State *s)
{
    uint8_t *c = s->config;
    int i;

    memset(c, 0, GT911_CONFIG_LEN);
    c[CFG_VERSION] = 'A';
    c[CFG_X_MAX] = s->width & 0xff;
    c[CFG_X_MAX + 1] = s->width >> 8;
    c[CFG_Y_MAX] = s->height & 0xff;
    c[CFG_Y_MAX + 1] = s->height >> 8;
    c[CFG_TOUCH_NUMBER] = s->max_contacts;
    c[CFG_MODULE_SWITCH1] = s->int_trigger & MODULE_SWITCH1_INT_MASK;
    c[CFG_SHAKE_COUNT] = 0x01;
    c[CFG_FILTER] = 0x08;
    c[CFG_LARGE_TOUCH] = 0x28;
    c[CFG_NOISE_REDUCTION] = 0x05;
    c[CFG_TOUCH_LEVEL] = 0x50;
    c[CFG_LEAVE_LEVEL] = 0x32;
    c[CFG_LOW_POWER] = 0x03;
    c[CFG_REFRESH_RATE] = 0x05;     /* 10 ms report period */
    /* Identity sensor and driver channel maps */
    for (i = 0; i < CFG_SENSOR_CH_NUM; i++) {
        c[CFG_SENSOR_CH0 + i] = i * 2;
    }
    for (i = 0; i < CFG_DRIVER_CH_NUM; i++) {
        c[CFG_DRIVER_CH0 + i] = i;
    }
    c[GT911_CONFIG_CHKSUM] = gt911_config_checksum(c);
    c[GT911_CONFIG_FRESH] = 1;
}

/* Volatile state lost on power-on reset and on RSTB */
static void gt911_reset_state(GT911State *s)
{
    timer_del(s->scan_timer);
    timer_del(s->pulse_timer);
    s->addr = 0;
    s->addr_bytes = 0;
    s->command = 0;
    s->cmd_check = 0;
    s->esd_check = 0;
    s->status = 0;
    s->screen_off = false;
    s->dirty = false;
    s->reported_down = false;
    memset(s->points, 0, sizeof(s->points));
}

static void gt911_reset_pin(void *opaque, int n, int level)
{
    GT911State *s = opaque;
    bool reset = !level;

    if (reset == s->in_reset) {
        return;
    }
    s->in_reset = reset;
    if (reset) {
        gt911_reset_state(s);
        return;
    }
    /*
     * End of reset: the INT level driven by the host selects the slave
     * address (high: 0x28/0x29 = 0x14, low: 0xBA/0xBB = 0x5D).
     */
    i2c_slave_set_address(I2C_SLAVE(s), s->int_sense ? GT911_ADDR_INT_HIGH
                                                     : GT911_ADDR_INT_LOW);
    gt911_set_int(s, false);
}

static void gt911_int_sense(void *opaque, int n, int level)
{
    GT911State *s = opaque;

    s->int_sense = level;
    /* A rising INT during sleep wakes the controller */
    if (s->screen_off && level) {
        s->screen_off = false;
        if (gt911_any_down(s)) {
            gt911_schedule_scan(s);
        }
    }
}

static void gt911_reset(DeviceState *dev)
{
    GT911State *s = GOODIX_GT911(dev);

    gt911_reset_state(s);
    gt911_build_config(s);
    memset(s->contacts, 0, sizeof(s->contacts));
    memset(&s->pointer, 0, sizeof(s->pointer));
    s->mtt_seen = false;
    s->cur_slot = 0;
    s->in_reset = false;
    gt911_set_int(s, false);
}

static void gt911_realize(DeviceState *dev, Error **errp)
{
    GT911State *s = GOODIX_GT911(dev);
    uint8_t addr = I2C_SLAVE(dev)->address;

    if (!s->width || !s->height || s->width > 0xffff || s->height > 0xffff) {
        error_setg(errp, "%s: width/height must be 1..65535", TYPE_GOODIX_GT911);
        return;
    }
    if (s->max_contacts < 1 || s->max_contacts > GT911_MAX_POINTS) {
        error_setg(errp, "%s: max-contacts must be 1..%d", TYPE_GOODIX_GT911,
                   GT911_MAX_POINTS);
        return;
    }
    if (s->int_trigger > GT911_INT_HIGH) {
        error_setg(errp, "%s: int-trigger must be 0..3", TYPE_GOODIX_GT911);
        return;
    }
    /*
     * Without an int-sense connection the address must survive a reset, so
     * the sensed INT level starts at the one that selects the current address.
     */
    s->int_sense = addr == GT911_ADDR_INT_HIGH;

    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gt911_scan, s);
    s->pulse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gt911_pulse_end, s);
    s->input = qemu_input_handler_register(dev, &gt911_input_handler);
}

static void gt911_unrealize(DeviceState *dev)
{
    GT911State *s = GOODIX_GT911(dev);

    qemu_input_handler_unregister(s->input);
    timer_free(s->scan_timer);
    timer_free(s->pulse_timer);
}

static void gt911_init(Object *obj)
{
    GT911State *s = GOODIX_GT911(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_out_named(dev, &s->int_out, GT911_GPIO_INT, 1);
    qdev_init_gpio_in_named(dev, gt911_int_sense, GT911_GPIO_INT_SENSE, 1);
    qdev_init_gpio_in_named(dev, gt911_reset_pin, GT911_GPIO_RESET, 1);
}

static const VMStateDescription vmstate_gt911_contact = {
    .name = "gt911-contact",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(down, GT911Contact),
        VMSTATE_UINT16(x, GT911Contact),
        VMSTATE_UINT16(y, GT911Contact),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_gt911 = {
    .name = TYPE_GOODIX_GT911,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, GT911State),
        VMSTATE_UINT16(addr, GT911State),
        VMSTATE_UINT8(addr_bytes, GT911State),
        VMSTATE_UINT8_ARRAY(config, GT911State, GT911_CONFIG_LEN),
        VMSTATE_UINT8(command, GT911State),
        VMSTATE_UINT8(cmd_check, GT911State),
        VMSTATE_UINT8(esd_check, GT911State),
        VMSTATE_UINT8(status, GT911State),
        VMSTATE_UINT8_ARRAY(points, GT911State,
                            GT911_MAX_POINTS * GT911_POINT_SIZE),
        VMSTATE_STRUCT_ARRAY(contacts, GT911State, GT911_MAX_POINTS, 1,
                             vmstate_gt911_contact, GT911Contact),
        VMSTATE_UINT32(cur_slot, GT911State),
        VMSTATE_BOOL(mtt_seen, GT911State),
        VMSTATE_STRUCT(pointer, GT911State, 1, vmstate_gt911_contact,
                       GT911Contact),
        VMSTATE_BOOL(dirty, GT911State),
        VMSTATE_BOOL(reported_down, GT911State),
        VMSTATE_BOOL(in_reset, GT911State),
        VMSTATE_BOOL(int_sense, GT911State),
        VMSTATE_BOOL(int_active, GT911State),
        VMSTATE_BOOL(screen_off, GT911State),
        VMSTATE_TIMER_PTR(scan_timer, GT911State),
        VMSTATE_TIMER_PTR(pulse_timer, GT911State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gt911_properties[] = {
    /* Panel size, mirrors touchscreen-size-x/y of the board DT */
    DEFINE_PROP_UINT32("width", GT911State, width, 800),
    DEFINE_PROP_UINT32("height", GT911State, height, 480),
    DEFINE_PROP_UINT32("max-contacts", GT911State, max_contacts, 5),
    /* Module_Switch1[1:0]: 0 rising, 1 falling (DT: IRQ_TYPE_EDGE_FALLING) */
    DEFINE_PROP_UINT32("int-trigger", GT911State, int_trigger, 1),
    /* Reported contact size (point bytes 5..6) */
    DEFINE_PROP_UINT32("touch-size", GT911State, touch_size, 0x20),
    DEFINE_PROP_UINT32("fw-version", GT911State, fw_version, 0x1060),
    DEFINE_PROP_UINT32("vendor-id", GT911State, vendor_id, 0),
    /* Width of the INT pulse in the edge trigger modes */
    DEFINE_PROP_UINT32("int-pulse-us", GT911State, int_pulse_us, 100),
};

static void gt911_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = gt911_event;
    k->recv = gt911_recv;
    k->send = gt911_send;
    dc->realize = gt911_realize;
    dc->unrealize = gt911_unrealize;
    dc->desc = "Goodix GT911 capacitive touch controller";
    dc->vmsd = &vmstate_gt911;
    device_class_set_legacy_reset(dc, gt911_reset);
    device_class_set_props(dc, gt911_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo gt911_info = {
    .name          = TYPE_GOODIX_GT911,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(GT911State),
    .instance_init = gt911_init,
    .class_init    = gt911_class_init,
};

static void gt911_register_types(void)
{
    type_register_static(&gt911_info);
}

type_init(gt911_register_types)
