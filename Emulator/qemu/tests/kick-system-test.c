/*
 * qtest for the i.MX8MM system control models of the kick machine:
 * SRC, GPC, IOMUXC/GPR, TMU, SNVS, system counter and the uSDHC CQHCI.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define SRC_BASE        0x30390000
#define GPC_BASE        0x303a0000
#define IOMUXC_BASE     0x30330000
#define GPR_BASE        0x30340000
#define TMU_BASE        0x30260000
#define SNVS_BASE       0x30370000
#define SYSCNT_RD       0x306a0000
#define SYSCNT_CMP      0x306b0000
#define SYSCNT_CTRL     0x306c0000
#define USDHC3_BASE     0x30b60000
#define GICD_BASE       0x38800000

/* GIC SPI n is QEMU GIC input n; pending state via GICD_ISPENDR */
static bool spi_pending(QTestState *qts, int spi)
{
    int intid = spi + 32;

    return qtest_readl(qts, GICD_BASE + 0x200 + (intid / 32) * 4) &
           (1u << (intid % 32));
}

static void test_src(void)
{
    QTestState *qts = qtest_init("-M kick");

    /* Only core 0 out of reset; power-on reset recorded in SRSR */
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x08), ==, 0x1);
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x5c), ==, 0x1);
    /* SRSR is write-1-to-clear */
    qtest_writel(qts, SRC_BASE + 0x5c, 0x1);
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x5c), ==, 0x0);
    /* SBMR2: internal boot, read-only */
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x70), ==, 0x02000000);
    qtest_writel(qts, SRC_BASE + 0x70, 0);
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x70), ==, 0x02000000);
    /* DDR controller held in reset until SPL releases it */
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x1000), ==, 0x8f00000f);
    qtest_writel(qts, SRC_BASE + 0x1000, 0x8f000000);
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x1000), ==, 0x8f000000);
    /* Core entry registers (GPR1..) are plain storage */
    qtest_writel(qts, SRC_BASE + 0x7c, 0x0024abcd);
    g_assert_cmphex(qtest_readl(qts, SRC_BASE + 0x7c), ==, 0x0024abcd);
    qtest_quit(qts);
}

static void test_gpc(void)
{
    QTestState *qts = qtest_init("-M kick");
    const uint32_t dispmix_pgc = 0xe80;     /* PU domain 10 */

    /* Peripheral domains start powered down (PGSR bit 0) */
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + dispmix_pgc + 0xc), ==, 1);
    /* Power-up request with PCR set completes and self-clears */
    qtest_writel(qts, GPC_BASE + dispmix_pgc, 1);
    qtest_writel(qts, GPC_BASE + 0xf8, 1u << 10);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + 0xf8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + dispmix_pgc + 0xc), ==, 0);
    /* Power down again */
    qtest_writel(qts, GPC_BASE + 0x104, 1u << 10);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + 0x104), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + dispmix_pgc + 0xc), ==, 1);
    /* ADB400 handshake: DISPMIX SYNC (bit 7) acknowledged at bit 25 */
    qtest_writel(qts, GPC_BASE + 0x1fc, 1u << 7);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + 0x1fc), ==,
                    (1u << 7) | (1u << 25));
    qtest_writel(qts, GPC_BASE + 0x1fc, 0);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + 0x1fc), ==, 0);
    /* CPU power-up trigger self-clears too */
    qtest_writel(qts, GPC_BASE + 0xf0, 0x2);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + 0xf0), ==, 0);
    qtest_quit(qts);
}

static void test_iomuxc(void)
{
    QTestState *qts = qtest_init("-M kick");

    /* UART4 RXD pad mux, pad control and a select input register */
    qtest_writel(qts, IOMUXC_BASE + 0x0228, 0x1);
    qtest_writel(qts, IOMUXC_BASE + 0x0490, 0x140);
    qtest_writel(qts, IOMUXC_BASE + 0x050c, 0x2);
    g_assert_cmphex(qtest_readl(qts, IOMUXC_BASE + 0x0228), ==, 0x1);
    g_assert_cmphex(qtest_readl(qts, IOMUXC_BASE + 0x0490), ==, 0x140);
    g_assert_cmphex(qtest_readl(qts, IOMUXC_BASE + 0x050c), ==, 0x2);
    qtest_writel(qts, GPR_BASE + 0x10, 0x8);
    g_assert_cmphex(qtest_readl(qts, GPR_BASE + 0x10), ==, 0x8);
    qtest_quit(qts);
}

static void test_tmu(void)
{
    QTestState *qts = qtest_init("-M kick");
    QDict *rsp;

    /* No reading while the monitor is disabled */
    g_assert_cmphex(qtest_readl(qts, TMU_BASE + 0x20), ==, 0);
    qtest_writel(qts, TMU_BASE + 0x00, 1u << 31);
    g_assert_cmphex(qtest_readl(qts, TMU_BASE + 0x20), ==, 45);

    /* Temperature settable at run time; threshold latches ITTE */
    rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': {"
                    " 'path': '/machine/soc/tmu', 'property': 'temperature',"
                    " 'value': 90 } }");
    g_assert(qdict_haskey(rsp, "return"));
    qobject_unref(rsp);
    g_assert_cmphex(qtest_readl(qts, TMU_BASE + 0x20), ==, 90);
    qtest_writel(qts, TMU_BASE + 0x10, (1u << 31) | 85);
    g_assert_cmphex(qtest_readl(qts, TMU_BASE + 0x0c), ==, 1u << 31);
    /* TIDR is write-1-to-clear; the condition latches it again */
    qtest_writel(qts, TMU_BASE + 0x10, 0);
    qtest_writel(qts, TMU_BASE + 0x0c, 1u << 31);
    g_assert_cmphex(qtest_readl(qts, TMU_BASE + 0x0c), ==, 0);
    qtest_quit(qts);
}

static void test_snvs(void)
{
    QTestState *qts = qtest_init("-M kick -rtc clock=vm");
    QDict *rsp;
    uint32_t lr1, lr2;

    /* SRTC holds still until SRTC_ENV, then counts 32768 per second */
    lr1 = qtest_readl(qts, SNVS_BASE + 0x54);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 10);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x54), ==, lr1);
    qtest_writel(qts, SNVS_BASE + 0x38, 0x1);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x38), ==, 0x1);

    /* Power key: press sets LPSR.SPO and raises SPI 4, BTN follows level */
    g_assert_false(spi_pending(qts, 4));
    rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': {"
                    " 'path': '/machine/soc/snvs', 'property': 'button-pressed',"
                    " 'value': true } }");
    qobject_unref(rsp);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x14) & (1u << 6), !=, 0);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x4c) & (1u << 18), !=, 0);
    g_assert_true(spi_pending(qts, 4));
    rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': {"
                    " 'path': '/machine/soc/snvs', 'property': 'button-pressed',"
                    " 'value': false } }");
    qobject_unref(rsp);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x14) & (1u << 6), ==, 0);
    qtest_writel(qts, SNVS_BASE + 0x4c, 1u << 18);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x4c) & (1u << 18), ==, 0);
    g_assert_false(spi_pending(qts, 4));

    /* Alarm: LPTAR one second ahead fires LPTA and SPI 19 */
    lr2 = qtest_readl(qts, SNVS_BASE + 0x54);
    qtest_writel(qts, SNVS_BASE + 0x58,
                 ((uint64_t)qtest_readl(qts, SNVS_BASE + 0x50) << 17 |
                  lr2 >> 15) + 1);
    qtest_writel(qts, SNVS_BASE + 0x38, 0x1 | 0x2 | 0x8);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x4c) & 1, ==, 0);
    qtest_clock_step(qts, 2 * NANOSECONDS_PER_SECOND);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x4c) & 1, ==, 1);
    g_assert_true(spi_pending(qts, 19));
    qtest_quit(qts);
}

static void test_sysctr(void)
{
    QTestState *qts = qtest_init("-M kick");
    uint32_t cv;

    /* Enabled at reset for direct kernel boot, base frequency 8 MHz */
    g_assert_cmphex(qtest_readl(qts, SYSCNT_CTRL + 0x00), ==, 0x3);
    g_assert_cmphex(qtest_readl(qts, SYSCNT_CTRL + 0x20), ==, 8000000);
    g_assert_cmphex(qtest_readl(qts, SYSCNT_CTRL + 0x28), ==, 0);

    cv = qtest_readl(qts, SYSCNT_RD + 0x08);
    qtest_clock_step(qts, 1000000);     /* 1 ms = 8000 ticks */
    g_assert_cmpuint(qtest_readl(qts, SYSCNT_RD + 0x08) - cv, ==, 8000);

    /* Compare frame 0: fire 10 us ahead, SPI 47 */
    cv = qtest_readl(qts, SYSCNT_RD + 0x08);
    qtest_writel(qts, SYSCNT_CMP + 0x20, cv + 80);
    qtest_writel(qts, SYSCNT_CMP + 0x24, 0);
    qtest_writel(qts, SYSCNT_CMP + 0x2c, 0x1);
    g_assert_cmphex(qtest_readl(qts, SYSCNT_CMP + 0x2c), ==, 0x1);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readl(qts, SYSCNT_CMP + 0x2c), ==, 0x5);
    g_assert_true(spi_pending(qts, 47));
    /* IMASK hides the interrupt, ISTAT stays */
    qtest_writel(qts, SYSCNT_CMP + 0x2c, 0x3);
    g_assert_cmphex(qtest_readl(qts, SYSCNT_CMP + 0x2c), ==, 0x7);
    g_assert_false(spi_pending(qts, 47));

    /* Disabling the counter freezes it */
    qtest_writel(qts, SYSCNT_CTRL + 0x00, 0x0);
    cv = qtest_readl(qts, SYSCNT_RD + 0x08);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readl(qts, SYSCNT_RD + 0x08), ==, cv);
    qtest_quit(qts);
}

static void test_cqhci(void)
{
    QTestState *qts = qtest_init("-M kick");
    uint32_t cq = USDHC3_BASE + 0x100;

    g_assert_cmphex(qtest_readl(qts, cq + 0x00), ==, 0x510);
    /* HALT completes immediately and reports HAC in CQIS */
    qtest_writel(qts, cq + 0x08, 0x1);
    qtest_writel(qts, cq + 0x0c, 0x1);
    g_assert_cmphex(qtest_readl(qts, cq + 0x0c), ==, 0x1);
    g_assert_cmphex(qtest_readl(qts, cq + 0x10), ==, 0x1);
    qtest_writel(qts, cq + 0x10, 0x1);
    g_assert_cmphex(qtest_readl(qts, cq + 0x10), ==, 0x0);
    qtest_writel(qts, cq + 0x0c, 0x0);
    g_assert_cmphex(qtest_readl(qts, cq + 0x0c), ==, 0x0);
    /* Version is read-only */
    qtest_writel(qts, cq + 0x00, 0);
    g_assert_cmphex(qtest_readl(qts, cq + 0x00), ==, 0x510);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/src", test_src);
    qtest_add_func("/kick/gpc", test_gpc);
    qtest_add_func("/kick/iomuxc", test_iomuxc);
    qtest_add_func("/kick/tmu", test_tmu);
    qtest_add_func("/kick/snvs", test_snvs);
    qtest_add_func("/kick/sysctr", test_sysctr);
    qtest_add_func("/kick/cqhci", test_cqhci);

    return g_test_run();
}
