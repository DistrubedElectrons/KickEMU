/*
 * qtest for the PCF85263A RTC model, DS1337 and native register views
 *
 * The RTC sits on I2C3 (0x68) of the kick-periph-test machine. The RTC
 * clock is the virtual clock starting at a fixed date, so time advances
 * only with clock_step.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/i2c.h"

#define I2C3_BASE       0x30a40000
#define RTC_ADDR        0x68
#define RTC_PATH        "/machine/peripheral/rtc"
#define NS_PER_SEC      1000000000LL

typedef struct {
    QTestState *qts;
    IMXI2C i2c;
} Fixture;

static void fixture_init(Fixture *f, const char *map)
{
    /* 2026-01-02 is a Friday */
    f->qts = qtest_initf("-M kick-periph-test "
                         "-rtc base=2026-01-02T03:04:05,clock=vm "
                         "-device pcf85263,id=rtc,bus=i2c-bus.1,address=0x%x,"
                         "register-map=%s", RTC_ADDR, map);
    imx_i2c_init(&f->i2c, f->qts, I2C3_BASE);
}

static void rtc_read(Fixture *f, uint8_t reg, uint8_t *buf, uint16_t len)
{
    f->i2c.parent.send(&f->i2c.parent, RTC_ADDR, &reg, 1);
    f->i2c.parent.recv(&f->i2c.parent, RTC_ADDR, buf, len);
}

static void rtc_write(Fixture *f, uint8_t reg, const uint8_t *data,
                      uint16_t len)
{
    uint8_t buf[32];

    buf[0] = reg;
    memcpy(&buf[1], data, len);
    f->i2c.parent.send(&f->i2c.parent, RTC_ADDR, buf, len + 1);
}

static void rtc_write8(Fixture *f, uint8_t reg, uint8_t val)
{
    rtc_write(f, reg, &val, 1);
}

static void test_ds1337_read(void)
{
    Fixture f;
    uint8_t r[16];

    fixture_init(&f, "ds1337");
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmphex(r[0], ==, 0x05);
    g_assert_cmphex(r[1], ==, 0x04);
    g_assert_cmphex(r[2], ==, 0x03);
    g_assert_cmphex(r[3], ==, 6);       /* day counter: Friday + 1 */
    g_assert_cmphex(r[4], ==, 0x02);
    g_assert_cmphex(r[5], ==, 0x01);
    g_assert_cmphex(r[6], ==, 0x26);
    g_assert_cmphex(r[0x0e], ==, 0x18); /* control reset value */
    g_assert_cmphex(r[0x0f], ==, 0x00); /* OSF clear: battery backed */

    qtest_clock_step(f.qts, 61 * NS_PER_SEC);
    rtc_read(&f, 0x00, r, 2);
    g_assert_cmphex(r[0], ==, 0x06);
    g_assert_cmphex(r[1], ==, 0x05);
    qtest_quit(f.qts);
}

static void test_ds1337_set(void)
{
    Fixture f;
    /* rtc-ds1307 style bulk write: 2031-12-31 23:59:58, day 4 */
    const uint8_t t[7] = { 0x58, 0x59, 0x23, 4, 0x31, 0x12, 0x31 };
    uint8_t r[7];

    fixture_init(&f, "ds1337");
    rtc_write(&f, 0x00, t, sizeof(t));
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmpmem(r, sizeof(r), t, sizeof(t));

    qtest_clock_step(f.qts, NS_PER_SEC + NS_PER_SEC / 2);
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmphex(r[0], ==, 0x59);

    qtest_clock_step(f.qts, NS_PER_SEC);
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmphex(r[0], ==, 0x00);
    g_assert_cmphex(r[1], ==, 0x00);
    g_assert_cmphex(r[2], ==, 0x00);
    g_assert_cmphex(r[3], ==, 5);
    g_assert_cmphex(r[4], ==, 0x01);
    g_assert_cmphex(r[5], ==, 0x01);
    g_assert_cmphex(r[6], ==, 0x32);

    /* 12 hour mode: 01:00:00 PM */
    rtc_write8(&f, 0x02, 0x40 | 0x20 | 0x01);
    rtc_read(&f, 0x02, r, 1);
    g_assert_cmphex(r[0], ==, 0x61);
    qtest_quit(f.qts);
}

static void test_ds1337_alarm(void)
{
    Fixture f;
    const uint8_t every_second[4] = { 0x80, 0x80, 0x80, 0x80 };
    uint8_t st;

    fixture_init(&f, "ds1337");
    qtest_irq_intercept_out_named(f.qts, RTC_PATH, "inta");
    rtc_write(&f, 0x07, every_second, sizeof(every_second));
    rtc_write8(&f, 0x0e, 0x04 | 0x01);          /* INTCN | A1IE */
    g_assert_true(qtest_get_irq(f.qts, 0));     /* INTA released */

    qtest_clock_step(f.qts, NS_PER_SEC);
    rtc_read(&f, 0x0f, &st, 1);
    g_assert_cmphex(st & 0x01, ==, 0x01);       /* A1F */
    g_assert_false(qtest_get_irq(f.qts, 0));

    rtc_write8(&f, 0x0f, 0x00);
    g_assert_true(qtest_get_irq(f.qts, 0));

    /* Oscillator stop: time frozen and OSF set */
    rtc_write8(&f, 0x0e, 0x80 | 0x04);
    rtc_read(&f, 0x0f, &st, 1);
    g_assert_cmphex(st & 0x80, ==, 0x80);
    qtest_quit(f.qts);
}

static void test_pcf_native(void)
{
    Fixture f;
    uint8_t r[8];
    const uint8_t stop_cpr[2] = { 0x01, 0xa4 };
    /* rtc-pcf85363 set_time: 100ths .. years, 2031-12-31 23:59:58 Wed */
    const uint8_t t[8] = { 0x00, 0x58, 0x59, 0x23, 0x31, 3, 0x12, 0x31 };

    fixture_init(&f, "pcf85263");
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmphex(r[1], ==, 0x05);
    g_assert_cmphex(r[2], ==, 0x04);
    g_assert_cmphex(r[3], ==, 0x03);
    g_assert_cmphex(r[4], ==, 0x02);
    g_assert_cmphex(r[5], ==, 5);       /* weekday */
    g_assert_cmphex(r[6], ==, 0x01);
    g_assert_cmphex(r[7], ==, 0x26);

    /* Stop, clear prescaler, write the time, start */
    rtc_write(&f, 0x2e, stop_cpr, sizeof(stop_cpr));
    rtc_write(&f, 0x00, t, sizeof(t));
    qtest_clock_step(f.qts, 5 * NS_PER_SEC);    /* stopped: no change */
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmpmem(r, sizeof(r), t, sizeof(t));
    rtc_write8(&f, 0x2e, 0x00);

    /* Alarm 1 on second 00, INTA as level output */
    qtest_irq_intercept_out_named(f.qts, RTC_PATH, "inta");
    rtc_write8(&f, 0x08, 0x00);
    rtc_write8(&f, 0x10, 0x01);         /* SEC_A1E */
    rtc_write8(&f, 0x27, 0x02);         /* INTAPM = INTA output */
    rtc_write8(&f, 0x29, 0x90);         /* ILP level | A1IE */
    g_assert_true(qtest_get_irq(f.qts, 0));

    qtest_clock_step(f.qts, NS_PER_SEC);
    rtc_read(&f, 0x2b, r, 1);
    g_assert_cmphex(r[0] & 0x20, ==, 0);
    qtest_clock_step(f.qts, NS_PER_SEC);
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmphex(r[1], ==, 0x00);
    g_assert_cmphex(r[7], ==, 0x32);
    g_assert_cmphex(r[5], ==, 4);
    rtc_read(&f, 0x2b, r, 1);
    g_assert_cmphex(r[0] & 0x20, ==, 0x20);     /* A1F */
    g_assert_false(qtest_get_irq(f.qts, 0));

    /* Flags clear by writing 0 */
    rtc_write8(&f, 0x2b, 0x00);
    g_assert_true(qtest_get_irq(f.qts, 0));
    qtest_quit(f.qts);
}

static void test_pcf_ds1337_driver_on_native(void)
{
    Fixture f;
    uint8_t r[7];

    /*
     * What the shipped kernel sees on the real board: the DS1337 time
     * layout read from the PCF85263 map starts at the 100ths register.
     */
    fixture_init(&f, "pcf85263");
    rtc_read(&f, 0x00, r, sizeof(r));
    g_assert_cmphex(r[0], ==, 0x00);    /* 100ths, read as seconds */
    g_assert_cmphex(r[1], ==, 0x05);    /* seconds, read as minutes */
    qtest_quit(f.qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/pcf85263/ds1337/read", test_ds1337_read);
    qtest_add_func("/kick/pcf85263/ds1337/set", test_ds1337_set);
    qtest_add_func("/kick/pcf85263/ds1337/alarm", test_ds1337_alarm);
    qtest_add_func("/kick/pcf85263/native/set-alarm", test_pcf_native);
    qtest_add_func("/kick/pcf85263/native/ds1337-layout",
                   test_pcf_ds1337_driver_on_native);
    return g_test_run();
}
