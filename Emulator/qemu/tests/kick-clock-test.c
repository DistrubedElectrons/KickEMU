/*
 * qtest for the i.MX8MM CCM and CCM_ANALOG (ANATOP) models
 *
 * Checks reset values, the SET/CLR/TOG register aliases of the CCM, the
 * PLL lock bit following the reset control, DIGPROG and the clock root
 * frequency seen by GPT1 (counting at its CCM-selected rate).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define CCM_BASE            0x30380000
#define ANATOP_BASE         0x30360000
#define GPT1_BASE           0x302d0000

#define CCM_CCGR(n)         (0x4000 + (n) * 0x10)
#define CCM_ROOT_GPT1       0xb580
#define CCM_ROOT_UART4      0xb080
#define SET                 0x4
#define CLR                 0x8
#define TOG                 0xc

#define ANA_ARM_PLL_GNRL    0x84
#define ANA_ARM_PLL_DIV     0x88
#define ANA_SYS_PLL1_GNRL   0x94
#define ANA_DIGPROG         0x800
#define PLL_LOCK            (1u << 31)
#define PLL_RST             (1u << 9)

static QTestState *start(void)
{
    return qtest_init("-M kick");
}

static void test_anatop_reset(void)
{
    QTestState *qts = start();

    /* i.MX8M family (0x82), Mini (0x41), revision 1.0 (0x10) */
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_DIGPROG), ==,
                    0x00824110);
    /* ARM PLL: 1.2 GHz (MDIV 300, PDIV 3, SDIV 1), out of reset, locked */
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_ARM_PLL_DIV), ==,
                    0x0012c031);
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL), ==,
                    0x00000a00 | PLL_LOCK);
    /* SYS_PLL1 with all fixed divider outputs enabled */
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_SYS_PLL1_GNRL), ==,
                    0x0aaaaa00 | PLL_LOCK);
    qtest_quit(qts);
}

static void test_anatop_lock(void)
{
    QTestState *qts = start();
    uint32_t v = qtest_readl(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL);

    /* Holding the PLL in reset drops LOCK */
    qtest_writel(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL, v & ~PLL_RST);
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL) &
                    PLL_LOCK, ==, 0);
    /* Releasing it locks again without a delay */
    qtest_writel(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL, v | PLL_RST);
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL) &
                    PLL_LOCK, ==, PLL_LOCK);
    /* LOCK is read-only: writing it while in reset does not set it */
    qtest_writel(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL,
                 (v & ~PLL_RST) | PLL_LOCK);
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_ARM_PLL_GNRL) &
                    PLL_LOCK, ==, 0);
    /* DIGPROG is read-only */
    qtest_writel(qts, ANATOP_BASE + ANA_DIGPROG, 0);
    g_assert_cmphex(qtest_readl(qts, ANATOP_BASE + ANA_DIGPROG), ==,
                    0x00824110);
    qtest_quit(qts);
}

static void test_ccm_aliases(void)
{
    QTestState *qts = start();
    uint32_t root = CCM_BASE + CCM_ROOT_UART4;

    /* Reset: gates on for domain 0, roots enabled on mux input 0 */
    g_assert_cmphex(qtest_readl(qts, CCM_BASE + CCM_CCGR(73)), ==, 0x3);
    g_assert_cmphex(qtest_readl(qts, root), ==, 0x10000000);

    qtest_writel(qts, root + SET, 0x01000000);
    g_assert_cmphex(qtest_readl(qts, root), ==, 0x11000000);
    qtest_writel(qts, root + CLR, 0x10000000);
    g_assert_cmphex(qtest_readl(qts, root), ==, 0x01000000);
    qtest_writel(qts, root + TOG, 0x10000003);
    g_assert_cmphex(qtest_readl(qts, root), ==, 0x11000003);
    /* The aliases read back the register value */
    g_assert_cmphex(qtest_readl(qts, root + SET), ==, 0x11000003);

    qtest_writel(qts, CCM_BASE + CCM_CCGR(73) + CLR, 0x3);
    g_assert_cmphex(qtest_readl(qts, CCM_BASE + CCM_CCGR(73)), ==, 0);
    qtest_quit(qts);
}

/*
 * GPT1 on CLKSRC = ipg_clk counts at GPT1_CLK_ROOT. Select SYS_PLL2_100M
 * (mux 1) / 100 = 1 MHz and check the counter advances 1000 per ms.
 */
static void test_gpt_root_frequency(void)
{
    QTestState *qts = start();
    uint32_t cnt;

    /* MUX 1 (sys_pll2_100m), PRE_PODF /4, POST_PODF /25 */
    qtest_writel(qts, CCM_BASE + CCM_ROOT_GPT1,
                 0x10000000 | (1 << 24) | (3 << 16) | 24);
    /* GPT CR: EN, ENMOD, CLKSRC = 1 (ipg_clk), free run */
    qtest_writel(qts, GPT1_BASE + 0x00, (1 << 0) | (1 << 1) | (1 << 6) |
                 (1 << 9));
    qtest_clock_step(qts, 1000000);     /* 1 ms */
    cnt = qtest_readl(qts, GPT1_BASE + 0x24);
    g_assert_cmpuint(cnt, >=, 999);
    g_assert_cmpuint(cnt, <=, 1001);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/anatop/reset", test_anatop_reset);
    qtest_add_func("/kick/anatop/pll-lock", test_anatop_lock);
    qtest_add_func("/kick/ccm/aliases", test_ccm_aliases);
    qtest_add_func("/kick/ccm/gpt-root-frequency", test_gpt_root_frequency);

    return g_test_run();
}
