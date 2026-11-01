/*
 * qtest for the Goodix GT911 touch controller model
 *
 * Runs on the kick-periph-test machine with the GT911 on I2C3 (0x14), the
 * bus and address of the KICK board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/i2c.h"
#include "hw/i2c/imx_i2c.h"
#include "qobject/qdict.h"

#define I2C3_BASE       0x30a40000
#define GT911_ADDR      0x14
#define TS_PATH         "/machine/peripheral/ts"

#define REG_CONFIG      0x8047
#define CONFIG_LEN      186
#define REG_ID          0x8140
#define REG_STATUS      0x814e
#define REG_POINT1      0x814f

/* Default report period: 5 + Refresh_Rate (5) ms */
#define SCAN_NS         (10 * 1000 * 1000)
#define PULSE_NS        (100 * 1000)

typedef struct {
    QTestState *qts;
    IMXI2C i2c;
} Fixture;

static void fixture_init(Fixture *f, const char *extra)
{
    f->qts = qtest_initf("-M kick-periph-test -device goodix-gt911,id=ts,"
                         "bus=i2c-bus.0,address=0x%x%s", GT911_ADDR,
                         extra ? extra : "");
    imx_i2c_init(&f->i2c, f->qts, I2C3_BASE);
}

static void gt_read_at(Fixture *f, uint8_t addr, uint16_t reg, uint8_t *buf,
                       uint16_t len)
{
    uint8_t a[2] = { reg >> 8, reg & 0xff };

    f->i2c.parent.send(&f->i2c.parent, addr, a, 2);
    f->i2c.parent.recv(&f->i2c.parent, addr, buf, len);
}

static void gt_read(Fixture *f, uint16_t reg, uint8_t *buf, uint16_t len)
{
    gt_read_at(f, GT911_ADDR, reg, buf, len);
}

static void gt_write8(Fixture *f, uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { reg >> 8, reg & 0xff, val };

    f->i2c.parent.send(&f->i2c.parent, GT911_ADDR, b, 3);
}

/* Address phase only: returns true if the slave acknowledged */
static bool i2c_probe(Fixture *f, uint8_t addr)
{
    uint8_t sr;

    qtest_writeb(f->qts, I2C3_BASE + I2CR_ADDR,
                 I2CR_IEN | I2CR_MSTA | I2CR_MTX);
    qtest_writeb(f->qts, I2C3_BASE + I2DR_ADDR, addr << 1);
    sr = qtest_readb(f->qts, I2C3_BASE + I2SR_ADDR);
    qtest_writeb(f->qts, I2C3_BASE + I2SR_ADDR, 0);
    qtest_writeb(f->qts, I2C3_BASE + I2CR_ADDR, I2CR_IEN);
    return !(sr & I2SR_RXAK);
}

static void mtt(Fixture *f, const char *type, int slot, int id)
{
    qtest_qmp_assert_success(f->qts,
        "{'execute': 'input-send-event', 'arguments': {'events': ["
        "{'type': 'mtt', 'data': {'type': %s, 'slot': %d, 'tracking-id': %d,"
        " 'axis': 'x', 'value': 0}}]}}", type, slot, id);
}

static void mtt_pos(Fixture *f, int slot, int id, int x, int y)
{
    qtest_qmp_assert_success(f->qts,
        "{'execute': 'input-send-event', 'arguments': {'events': ["
        "{'type': 'mtt', 'data': {'type': 'data', 'slot': %d,"
        " 'tracking-id': %d, 'axis': 'x', 'value': %d}},"
        "{'type': 'mtt', 'data': {'type': 'data', 'slot': %d,"
        " 'tracking-id': %d, 'axis': 'y', 'value': %d}}]}}",
        slot, id, x, slot, id, y);
}

static void test_id(void)
{
    Fixture f;
    uint8_t buf[11];

    fixture_init(&f, NULL);
    gt_read(&f, REG_ID, buf, sizeof(buf));
    g_assert_cmpmem(buf, 4, "911\0", 4);
    g_assert_cmphex(buf[4] | (buf[5] << 8), ==, 0x1060);    /* fw version */
    g_assert_cmpuint(buf[6] | (buf[7] << 8), ==, 800);      /* x res */
    g_assert_cmpuint(buf[8] | (buf[9] << 8), ==, 480);      /* y res */
    qtest_quit(f.qts);
}

static void test_config(void)
{
    Fixture f;
    uint8_t cfg[CONFIG_LEN];
    uint8_t sum = 0;
    int i;

    fixture_init(&f, NULL);
    gt_read(&f, REG_CONFIG, cfg, sizeof(cfg));
    g_assert_cmpuint(cfg[1] | (cfg[2] << 8), ==, 800);
    g_assert_cmpuint(cfg[3] | (cfg[4] << 8), ==, 480);
    g_assert_cmpuint(cfg[5] & 0x0f, ==, 5);     /* touch number */
    g_assert_cmpuint(cfg[6] & 0x03, ==, 1);     /* falling edge */
    for (i = 0; i < CONFIG_LEN - 2; i++) {
        sum += cfg[i];
    }
    g_assert_cmphex((uint8_t)(~sum + 1), ==, cfg[CONFIG_LEN - 2]);
    g_assert_cmpuint(cfg[CONFIG_LEN - 1], ==, 1);   /* Config_Fresh */

    /* Status starts empty */
    gt_read(&f, REG_STATUS, cfg, 1);
    g_assert_cmphex(cfg[0], ==, 0);
    qtest_quit(f.qts);
}

static void test_config_props(void)
{
    Fixture f;
    uint8_t cfg[7];

    fixture_init(&f, ",width=1024,height=600,max-contacts=2,int-trigger=0");
    gt_read(&f, REG_CONFIG, cfg, sizeof(cfg));
    g_assert_cmpuint(cfg[1] | (cfg[2] << 8), ==, 1024);
    g_assert_cmpuint(cfg[3] | (cfg[4] << 8), ==, 600);
    g_assert_cmpuint(cfg[5], ==, 2);
    g_assert_cmpuint(cfg[6] & 3, ==, 0);
    qtest_quit(f.qts);
}

static void test_touch_sequence(void)
{
    Fixture f;
    uint8_t buf[1 + 2 * 8];

    fixture_init(&f, NULL);
    qtest_irq_intercept_out_named(f.qts, TS_PATH, "int");

    /* One finger down at the centre */
    mtt(&f, "begin", 0, 0);
    mtt_pos(&f, 0, 0, 0x4000, 0x4000);

    /* Nothing is reported before the next scan */
    gt_read(&f, REG_STATUS, buf, 1);
    g_assert_cmphex(buf[0], ==, 0);

    qtest_clock_step(f.qts, SCAN_NS);
    gt_read(&f, REG_STATUS, buf, 9);
    g_assert_cmphex(buf[0], ==, 0x81);          /* buffer ready, 1 point */
    g_assert_cmpuint(buf[1], ==, 0);            /* track id */
    g_assert_cmpuint(buf[2] | (buf[3] << 8), ==, 400);
    g_assert_cmpuint(buf[4] | (buf[5] << 8), ==, 240);
    g_assert_cmpuint(buf[6] | (buf[7] << 8), ==, 0x20);
    /* Falling edge mode: INT low during the pulse, high afterwards */
    g_assert_false(qtest_get_irq(f.qts, 0));
    qtest_clock_step(f.qts, PULSE_NS);
    g_assert_true(qtest_get_irq(f.qts, 0));

    /* No new report while the host has not cleared the buffer */
    qtest_clock_step(f.qts, SCAN_NS);
    g_assert_true(qtest_get_irq(f.qts, 0));

    /*
     * Host clears: the finger is still down and is reported on the next
     * scan, which keeps its 10 ms phase (next one is 0.1 ms - pulse early).
     */
    gt_write8(&f, REG_STATUS, 0);
    qtest_clock_step(f.qts, SCAN_NS - PULSE_NS);
    g_assert_false(qtest_get_irq(f.qts, 0));
    qtest_clock_step(f.qts, PULSE_NS);
    g_assert_true(qtest_get_irq(f.qts, 0));
    gt_read(&f, REG_STATUS, buf, 1);
    g_assert_cmphex(buf[0], ==, 0x81);

    /* Second finger in slot 3 */
    gt_write8(&f, REG_STATUS, 0);
    mtt(&f, "begin", 3, 7);
    mtt_pos(&f, 3, 7, 0x7fff, 0);
    qtest_clock_step(f.qts, SCAN_NS);
    gt_read(&f, REG_STATUS, buf, sizeof(buf));
    g_assert_cmphex(buf[0], ==, 0x82);
    g_assert_cmpuint(buf[1], ==, 0);
    g_assert_cmpuint(buf[9], ==, 3);            /* track id = slot */
    g_assert_cmpuint(buf[10] | (buf[11] << 8), ==, 799);
    g_assert_cmpuint(buf[12] | (buf[13] << 8), ==, 0);

    /* Both fingers up: one report with zero points, then silence */
    gt_write8(&f, REG_STATUS, 0);
    mtt(&f, "end", 0, -1);
    mtt(&f, "end", 3, -1);
    qtest_clock_step(f.qts, SCAN_NS);
    gt_read(&f, REG_STATUS, buf, 1);
    g_assert_cmphex(buf[0], ==, 0x80);
    qtest_clock_step(f.qts, PULSE_NS);
    gt_write8(&f, REG_STATUS, 0);
    qtest_clock_step(f.qts, 5 * SCAN_NS);
    gt_read(&f, REG_STATUS, buf, 1);
    g_assert_cmphex(buf[0], ==, 0);
    g_assert_true(qtest_get_irq(f.qts, 0));
    qtest_quit(f.qts);
}

static void test_pointer(void)
{
    Fixture f;
    uint8_t buf[9];

    fixture_init(&f, NULL);
    qtest_qmp_assert_success(f.qts,
        "{'execute': 'input-send-event', 'arguments': {'events': ["
        "{'type': 'abs', 'data': {'axis': 'x', 'value': 8192}},"
        "{'type': 'abs', 'data': {'axis': 'y', 'value': 24576}},"
        "{'type': 'btn', 'data': {'button': 'left', 'down': true}}]}}");
    qtest_clock_step(f.qts, SCAN_NS);
    gt_read(&f, REG_STATUS, buf, sizeof(buf));
    g_assert_cmphex(buf[0], ==, 0x81);
    g_assert_cmpuint(buf[2] | (buf[3] << 8), ==, 200);
    g_assert_cmpuint(buf[4] | (buf[5] << 8), ==, 360);
    qtest_quit(f.qts);
}

static void test_level_trigger(void)
{
    Fixture f;

    /* Low level: INT held low until the host clears the buffer status */
    fixture_init(&f, ",int-trigger=2");
    qtest_irq_intercept_out_named(f.qts, TS_PATH, "int");
    mtt(&f, "begin", 0, 0);
    mtt_pos(&f, 0, 0, 100, 100);
    qtest_clock_step(f.qts, SCAN_NS + 5 * PULSE_NS);
    g_assert_false(qtest_get_irq(f.qts, 0));
    gt_write8(&f, REG_STATUS, 0);
    g_assert_true(qtest_get_irq(f.qts, 0));
    qtest_quit(f.qts);
}

static void test_reset_address_select(void)
{
    Fixture f;
    uint8_t id[4];

    fixture_init(&f, NULL);
    /* Held in reset: no acknowledge */
    qtest_set_irq_in(f.qts, TS_PATH, "reset", 0, 0);
    g_assert_false(i2c_probe(&f, GT911_ADDR));

    /* INT low at the end of reset selects 0x5d */
    qtest_set_irq_in(f.qts, TS_PATH, "int-sense", 0, 0);
    qtest_set_irq_in(f.qts, TS_PATH, "reset", 0, 1);
    g_assert_false(i2c_probe(&f, GT911_ADDR));
    gt_read_at(&f, 0x5d, REG_ID, id, sizeof(id));
    g_assert_cmpmem(id, 4, "911\0", 4);

    /* INT high selects 0x14 again, as the Linux driver does */
    qtest_set_irq_in(f.qts, TS_PATH, "reset", 0, 0);
    qtest_set_irq_in(f.qts, TS_PATH, "int-sense", 0, 1);
    qtest_set_irq_in(f.qts, TS_PATH, "reset", 0, 1);
    gt_read(&f, REG_ID, id, sizeof(id));
    g_assert_cmpmem(id, 4, "911\0", 4);
    qtest_quit(f.qts);
}

static void test_config_write(void)
{
    Fixture f;
    uint8_t cfg[CONFIG_LEN];
    uint8_t msg[2 + CONFIG_LEN];
    uint8_t sum = 0;
    int i;

    fixture_init(&f, NULL);
    gt_read(&f, REG_CONFIG, cfg, sizeof(cfg));

    /* New resolution with a valid checksum and Config_Fresh */
    cfg[1] = 0x00;
    cfg[2] = 0x04;      /* 1024 */
    for (i = 0; i < CONFIG_LEN - 2; i++) {
        sum += cfg[i];
    }
    cfg[CONFIG_LEN - 2] = ~sum + 1;
    cfg[CONFIG_LEN - 1] = 1;
    msg[0] = REG_CONFIG >> 8;
    msg[1] = REG_CONFIG & 0xff;
    memcpy(&msg[2], cfg, CONFIG_LEN);
    f.i2c.parent.send(&f.i2c.parent, GT911_ADDR, msg, sizeof(msg));
    gt_read(&f, 0x8146, cfg, 2);
    g_assert_cmpuint(cfg[0] | (cfg[1] << 8), ==, 1024);
    qtest_quit(f.qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/gt911/id", test_id);
    qtest_add_func("/kick/gt911/config", test_config);
    qtest_add_func("/kick/gt911/config-props", test_config_props);
    qtest_add_func("/kick/gt911/config-write", test_config_write);
    qtest_add_func("/kick/gt911/touch-sequence", test_touch_sequence);
    qtest_add_func("/kick/gt911/pointer", test_pointer);
    qtest_add_func("/kick/gt911/level-trigger", test_level_trigger);
    qtest_add_func("/kick/gt911/reset-address-select",
                   test_reset_address_select);
    return g_test_run();
}
