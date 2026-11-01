/*
 * qtest for the PCA9534 GPIO expander model (I2C2, 0x20)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/i2c.h"
#include "qobject/qdict.h"

#define I2C2_BASE       0x30a30000
#define EXP_ADDR        0x20
#define EXP_PATH        "/machine/peripheral/exp"

typedef struct {
    QTestState *qts;
    IMXI2C i2c;
} Fixture;

static void fixture_init(Fixture *f)
{
    f->qts = qtest_initf("-M kick-periph-test -device pca9534,id=exp,"
                         "bus=i2c-bus.0,address=0x%x", EXP_ADDR);
    imx_i2c_init(&f->i2c, f->qts, I2C2_BASE);
}

static uint8_t exp_read(Fixture *f, uint8_t reg)
{
    uint8_t v;

    f->i2c.parent.send(&f->i2c.parent, EXP_ADDR, &reg, 1);
    f->i2c.parent.recv(&f->i2c.parent, EXP_ADDR, &v, 1);
    return v;
}

static void exp_write(Fixture *f, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };

    f->i2c.parent.send(&f->i2c.parent, EXP_ADDR, b, 2);
}

static void test_reset(void)
{
    Fixture f;

    fixture_init(&f);
    g_assert_cmphex(exp_read(&f, 0), ==, 0xff);     /* pulled-up inputs */
    g_assert_cmphex(exp_read(&f, 1), ==, 0xff);
    g_assert_cmphex(exp_read(&f, 2), ==, 0x00);
    g_assert_cmphex(exp_read(&f, 3), ==, 0xff);
    qtest_quit(f.qts);
}

static void test_outputs_and_polarity(void)
{
    Fixture f;

    fixture_init(&f);
    qtest_irq_intercept_out(f.qts, EXP_PATH);
    /* GPIO hogs: pins 4..7 outputs driven low */
    exp_write(&f, 1, 0x0f);
    exp_write(&f, 3, 0x0f);
    g_assert_cmphex(exp_read(&f, 0), ==, 0x0f);
    g_assert_false(qtest_get_irq(f.qts, 4));
    g_assert_false(qtest_get_irq(f.qts, 7));

    exp_write(&f, 1, 0x8f);
    g_assert_true(qtest_get_irq(f.qts, 7));
    g_assert_cmphex(exp_read(&f, 0), ==, 0x8f);

    /* Polarity inversion applies to the Input Port only */
    exp_write(&f, 2, 0x0f);
    g_assert_cmphex(exp_read(&f, 0), ==, 0x80);
    g_assert_cmphex(exp_read(&f, 1), ==, 0x8f);
    qtest_quit(f.qts);
}

static void test_button_interrupt(void)
{
    Fixture f;

    fixture_init(&f);
    exp_write(&f, 3, 0x0f);
    exp_write(&f, 1, 0x00);
    exp_read(&f, 0);
    qtest_irq_intercept_out_named(f.qts, EXP_PATH, "int");

    /* Home button (pin 2, active low) pressed from the host via QOM */
    qtest_qmp_assert_success(f.qts, "{'execute': 'qom-set', 'arguments': {"
                             "'path': '" EXP_PATH "', 'property': "
                             "'input-levels', 'value': 251}}");
    g_assert_false(qtest_get_irq(f.qts, 0));    /* INT asserted (low) */
    g_assert_cmphex(exp_read(&f, 0) & 0x0f, ==, 0x0b);
    g_assert_true(qtest_get_irq(f.qts, 0));     /* cleared by the read */

    /* Release through the pin GPIO input; returning to the read state */
    qtest_set_irq_in(f.qts, EXP_PATH, NULL, 2, 1);
    g_assert_false(qtest_get_irq(f.qts, 0));
    qtest_set_irq_in(f.qts, EXP_PATH, NULL, 2, 0);
    g_assert_true(qtest_get_irq(f.qts, 0));

    /* Changes on output pins never interrupt */
    qtest_set_irq_in(f.qts, EXP_PATH, NULL, 6, 0);
    g_assert_true(qtest_get_irq(f.qts, 0));
    qtest_quit(f.qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/pca9534/reset", test_reset);
    qtest_add_func("/kick/pca9534/outputs-polarity", test_outputs_and_polarity);
    qtest_add_func("/kick/pca9534/button-interrupt", test_button_interrupt);
    return g_test_run();
}
