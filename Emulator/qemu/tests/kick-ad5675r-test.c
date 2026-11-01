/*
 * qtest for the AD5671R CV/Gate DAC model (I2C4, 0x0c)
 *
 * Uses the same frames as Kick/modules/hardware/source/kick_I2c.cpp and
 * checks the JSON lines written to the chardev.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/i2c.h"

#define I2C4_BASE       0x30a50000
#define DAC_ADDR        0x0c
#define DAC_PATH        "/machine/peripheral/cv"

typedef struct {
    QTestState *qts;
    IMXI2C i2c;
    char *log;
} Fixture;

static void fixture_init(Fixture *f)
{
    int fd = g_file_open_tmp("kick-cv-XXXXXX", &f->log, NULL);

    g_assert(fd >= 0);
    close(fd);
    f->qts = qtest_initf("-M kick-periph-test -chardev file,id=cvout,path=%s "
                         "-device ad5675r,id=cv,bus=i2c-bus.2,address=0x%x,"
                         "chardev=cvout", f->log, DAC_ADDR);
    imx_i2c_init(&f->i2c, f->qts, I2C4_BASE);
}

static void fixture_done(Fixture *f)
{
    qtest_quit(f->qts);
    unlink(f->log);
    g_free(f->log);
}

static void dac_frame(Fixture *f, uint8_t cmd, uint16_t data)
{
    uint8_t b[3] = { cmd, data >> 8, data & 0xff };

    f->i2c.parent.send(&f->i2c.parent, DAC_ADDR, b, 3);
}

/* kick_I2c.cpp: buffer[1] = v >> 4, buffer[2] = (v << 4) & 0xf0 */
static void dac_set(Fixture *f, uint8_t cmd, uint16_t code)
{
    dac_frame(f, cmd, (code << 4) & 0xfff0);
}

static char *last_line(Fixture *f)
{
    g_autofree char *content = NULL;
    g_auto(GStrv) lines = NULL;
    int n;

    g_assert(g_file_get_contents(f->log, &content, NULL, NULL));
    lines = g_strsplit(content, "\n", -1);
    n = g_strv_length(lines);
    g_assert_cmpint(n, >=, 2);
    return g_strdup(lines[n - 2]);  /* last one is empty after the \n */
}

static int line_count(Fixture *f)
{
    g_autofree char *content = NULL;
    int n = 0;
    char *p;

    g_assert(g_file_get_contents(f->log, &content, NULL, NULL));
    for (p = content; *p; p++) {
        n += *p == '\n';
    }
    return n;
}

static void test_write_and_report(void)
{
    Fixture f;
    g_autofree char *l1 = NULL, *l2 = NULL;

    fixture_init(&f);
    dac_set(&f, 0x30 | 5, 2048);
    l1 = last_line(&f);
    g_assert_cmpstr(l1, ==, "{\"ch\":5,\"dac\":2048,\"volts\":2.500}");

    /* setVoltageOn: 0xffff -> full scale */
    dac_frame(&f, 0x30 | 0, 0xffff);
    l2 = last_line(&f);
    g_assert_cmpstr(l2, ==, "{\"ch\":0,\"dac\":4095,\"volts\":4.999}");
    fixture_done(&f);
}

static void test_readback(void)
{
    Fixture f;
    uint8_t rx[16];
    int ch;

    fixture_init(&f);
    /* writeAllChannelVoltage with distinct values */
    for (ch = 0; ch < 8; ch++) {
        dac_set(&f, 0x30 | ch, 100 * (ch + 1));
    }
    /* readAllchannelVoltage: 0x90 then a 16 byte read */
    dac_frame(&f, 0x90, 0);
    f.i2c.parent.recv(&f.i2c.parent, DAC_ADDR, rx, sizeof(rx));
    for (ch = 0; ch < 8; ch++) {
        uint16_t v = (rx[ch * 2] << 8 | rx[ch * 2 + 1]) >> 4;

        g_assert_cmpuint(v, ==, 100 * (ch + 1));
        g_assert_cmphex(rx[ch * 2 + 1] & 0x0f, ==, 0);
    }
    /* readVoltage(ch 6): readback starts at the addressed channel */
    dac_frame(&f, 0x90 | 6, 0);
    f.i2c.parent.recv(&f.i2c.parent, DAC_ADDR, rx, 4);
    g_assert_cmpuint((rx[0] << 8 | rx[1]) >> 4, ==, 700);
    g_assert_cmpuint((rx[2] << 8 | rx[3]) >> 4, ==, 800);
    fixture_done(&f);
}

static void test_ldac_power_reset(void)
{
    Fixture f;
    g_autofree char *l1 = NULL, *l2 = NULL, *l3 = NULL;
    int n;

    fixture_init(&f);
    /* LDAC high: input register writes do not reach the DAC */
    qtest_set_irq_in(f.qts, DAC_PATH, "ldac", 0, 1);
    n = line_count(&f);
    dac_set(&f, 0x10 | 2, 1000);
    g_assert_cmpint(line_count(&f), ==, n);
    dac_frame(&f, 0x20 | 2, 0);                 /* update DAC 2 */
    l1 = last_line(&f);
    g_assert_cmpstr(l1, ==, "{\"ch\":2,\"dac\":1000,\"volts\":1.221}");

    /* Power down DAC 2 (1 kOhm to GND): 0 V, code kept */
    dac_frame(&f, 0x40, 1 << 4);
    l2 = last_line(&f);
    g_assert_cmpstr(l2, ==, "{\"ch\":2,\"dac\":1000,\"volts\":0.000}");

    /* Software reset back to zero scale, powered up */
    dac_frame(&f, 0x60, 0x1234);
    l3 = last_line(&f);
    g_assert_cmpstr(l3, ==, "{\"ch\":2,\"dac\":0,\"volts\":0.000}");
    fixture_done(&f);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/ad5675r/write-report", test_write_and_report);
    qtest_add_func("/kick/ad5675r/readback", test_readback);
    qtest_add_func("/kick/ad5675r/ldac-power-reset", test_ldac_power_reset);
    return g_test_run();
}
