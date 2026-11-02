/*
 * qtest for the i.MX8MM PWM of the kick machine: the register sequence of
 * Linux' pwm-imx27 driver (software reset, FIFO room, period, sample, enable)
 * and the duty the host reads to dim the panel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define PWM1_BASE   0x30660000
#define PWMCR       0x00
#define PWMSR       0x04
#define PWMSAR      0x0c
#define PWMPR       0x10

#define CR_EN       (1u << 0)
#define CR_SWR      (1u << 3)
#define CR_POUTC_INV (1u << 18)
#define SR_FIFOAV   0x7u
#define SR_FE       (1u << 3)

static uint32_t duty(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': {"
                                " 'path': '/machine/soc/pwm1',"
                                " 'property': 'duty-permille' } }");
    uint32_t d = qdict_get_int(rsp, "return");

    qobject_unref(rsp);
    return d;
}

static void test_backlight_sequence(void)
{
    QTestState *qts = qtest_init("-M kick");

    /* Reset values; disabled means no light */
    g_assert_cmphex(qtest_readl(qts, PWM1_BASE + PWMPR), ==, 0xfffe);
    g_assert_cmpuint(duty(qts), ==, 0);

    /* pwm-imx27: software reset must complete, the FIFO must have room */
    qtest_writel(qts, PWM1_BASE + PWMCR, CR_SWR);
    g_assert_cmphex(qtest_readl(qts, PWM1_BASE + PWMCR) & CR_SWR, ==, 0);
    g_assert_cmphex(qtest_readl(qts, PWM1_BASE + PWMSR) & SR_FIFOAV, ==, 0);
    g_assert_cmphex(qtest_readl(qts, PWM1_BASE + PWMSR) & SR_FE, ==, SR_FE);

    /* 1 kHz period (DT: 1000000 ns) from 24 MHz: PR = 24000 - 2; 80 % duty */
    qtest_writel(qts, PWM1_BASE + PWMPR, 23998);
    qtest_writel(qts, PWM1_BASE + PWMSAR, 19200);
    g_assert_cmpuint(duty(qts), ==, 0);     /* not enabled yet */
    qtest_writel(qts, PWM1_BASE + PWMCR, CR_EN);
    g_assert_cmpuint(duty(qts), ==, 800);

    /* Inverted output polarity */
    qtest_writel(qts, PWM1_BASE + PWMCR, CR_EN | CR_POUTC_INV);
    g_assert_cmpuint(duty(qts), ==, 200);

    /* Software reset drops everything back, including the enable */
    qtest_writel(qts, PWM1_BASE + PWMCR, CR_SWR);
    g_assert_cmpuint(duty(qts), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PWM1_BASE + PWMSAR), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/kick/pwm/backlight-sequence", test_backlight_sequence);
    return g_test_run();
}
