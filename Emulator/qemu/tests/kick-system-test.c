/*
 * qtest for the i.MX8MM system control models of the kick machine:
 * SRC, GPC, IOMUXC/GPR, TMU (and its interrupt), SNVS (button, long-press
 * power off, LP and HP alarms), system counter, GPIO1 per-pin interrupts,
 * APBH DMA reset, the uSDHC CQHCI and the uSDHC auto CMD23.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
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
#define GPIO1_BASE      0x30200000
#define APBH_BASE       0x33000000
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
    /* Power-up request (PCR clear, as ATF does) completes and self-clears */
    qtest_writel(qts, GPC_BASE + 0xf8, 1u << 10);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + 0xf8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + dispmix_pgc + 0xc), ==, 0);
    /* Power down is ignored unless PCR allows it */
    qtest_writel(qts, GPC_BASE + 0x104, 1u << 10);
    g_assert_cmphex(qtest_readl(qts, GPC_BASE + dispmix_pgc + 0xc), ==, 0);
    qtest_writel(qts, GPC_BASE + dispmix_pgc, 1);
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

static void tmu_set_temp(QTestState *qts, int t)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': {"
                           " 'path': '/machine/soc/tmu', 'property': "
                           "'temperature', 'value': %d } }", t);

    g_assert(qdict_haskey(rsp, "return"));
    qobject_unref(rsp);
}

/* TMU threshold interrupt on SPI 49 when enabled in TIER */
static void test_tmu_irq(void)
{
    QTestState *qts = qtest_init("-M kick");

    qtest_writel(qts, TMU_BASE + 0x00, 1u << 31);
    qtest_writel(qts, TMU_BASE + 0x10, (1u << 31) | 85);   /* ITTE at 85 C */
    tmu_set_temp(qts, 90);
    g_assert_cmphex(qtest_readl(qts, TMU_BASE + 0x0c), ==, 1u << 31);
    g_assert_false(spi_pending(qts, 49));
    qtest_writel(qts, TMU_BASE + 0x08, 1u << 31);           /* TIER.ITTEIE */
    g_assert_true(spi_pending(qts, 49));
    qtest_writel(qts, TMU_BASE + 0x10, 0);
    qtest_writel(qts, TMU_BASE + 0x0c, 1u << 31);
    g_assert_false(spi_pending(qts, 49));
    qtest_quit(qts);
}

/* GPIO1 pins 0..7: INT0 is SPI 63 down to INT7 at SPI 56 */
static void test_gpio1_pin_irq(void)
{
    QTestState *qts = qtest_init("-M kick");
    static const int pins[] = { 0, 3, 7 };

    qtest_writel(qts, GPIO1_BASE + 0x1c, 0x1ff);      /* EDGE_SEL: both */
    qtest_writel(qts, GPIO1_BASE + 0x14, 0x1ff);      /* IMR */
    for (int i = 0; i < ARRAY_SIZE(pins); i++) {
        int p = pins[i];

        /* Start from low: GPIO1_IO07 idles high (PCA9534 INT, open drain) */
        qtest_set_irq_in(qts, "/machine/soc/gpio1", NULL, p, 0);
        qtest_writel(qts, GPIO1_BASE + 0x18, 1u << p);
        qtest_set_irq_in(qts, "/machine/soc/gpio1", NULL, p, 1);
        for (int spi = 56; spi <= 63; spi++) {
            g_assert_cmpint(spi_pending(qts, spi), ==, spi == 63 - p);
        }
        g_assert_true(spi_pending(qts, 64));
        qtest_writel(qts, GPIO1_BASE + 0x18, 1u << p);    /* ISR W1C */
        g_assert_false(spi_pending(qts, 63 - p));
        qtest_set_irq_in(qts, "/machine/soc/gpio1", NULL, p, 0);
        qtest_writel(qts, GPIO1_BASE + 0x18, 1u << p);
    }
    /* Pin 8 has only the combined interrupt */
    qtest_set_irq_in(qts, "/machine/soc/gpio1", NULL, 8, 1);
    g_assert_true(spi_pending(qts, 64));
    for (int spi = 56; spi <= 63; spi++) {
        g_assert_false(spi_pending(qts, spi));
    }
    /* A masked pin raises nothing */
    qtest_writel(qts, GPIO1_BASE + 0x18, 0xffffffff);
    qtest_writel(qts, GPIO1_BASE + 0x14, 0);
    qtest_set_irq_in(qts, "/machine/soc/gpio1", NULL, 1, 1);
    g_assert_false(spi_pending(qts, 62));
    qtest_quit(qts);
}

static void snvs_button(QTestState *qts, bool pressed)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': {"
                           " 'path': '/machine/soc/snvs', 'property': "
                           "'button-pressed', 'value': %i } }", pressed);

    g_assert(qdict_haskey(rsp, "return"));
    qobject_unref(rsp);
}

static bool vm_shut_down(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'query-status' }");
    bool down = !strcmp(qdict_get_str(qdict_get_qdict(rsp, "return"),
                                      "status"), "shutdown");

    qobject_unref(rsp);
    return down;
}

#define SEC NANOSECONDS_PER_SECOND

/* Holding the ON/OFF button past LPCR.BTN_PRESS_TIME powers off */
static void test_snvs_long_press(void)
{
    static const struct {
        uint32_t bpt;
        int64_t off_after;      /* 0: never */
    } cases[] = { { 0, 5 }, { 1, 10 }, { 2, 15 }, { 3, 0 } };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = qtest_init("-M kick -action shutdown=pause");
        int64_t t = cases[i].off_after;

        qtest_writel(qts, SNVS_BASE + 0x38, cases[i].bpt << 16);
        /* A short push is only a key press */
        snvs_button(qts, true);
        qtest_clock_step(qts, SEC);
        snvs_button(qts, false);
        qtest_clock_step(qts, 20 * SEC);
        g_assert_false(vm_shut_down(qts));

        snvs_button(qts, true);
        if (t) {
            qtest_clock_step(qts, t * SEC - SEC / 10);
            g_assert_false(vm_shut_down(qts));
            qtest_clock_step(qts, SEC / 5);
            g_assert_true(vm_shut_down(qts));
        } else {
            qtest_clock_step(qts, 30 * SEC);
            g_assert_false(vm_shut_down(qts));
        }
        qtest_quit(qts);
    }
}

/* HP alarm: HPTAMR:HPTALR against the HP RTC, HPSR.HPTA, SPI 19 */
static void test_snvs_hp_alarm(void)
{
    QTestState *qts = qtest_init("-M kick -rtc clock=vm");
    uint64_t now, alarm;

    qtest_writel(qts, SNVS_BASE + 0x08, 0x1);                 /* RTC_EN */
    qtest_clock_step(qts, SEC);
    now = (uint64_t)qtest_readl(qts, SNVS_BASE + 0x24) << 32 |
          qtest_readl(qts, SNVS_BASE + 0x28);
    g_assert_cmpuint(now, >=, 32768);
    alarm = now + 32768;                                      /* +1 s */
    qtest_writel(qts, SNVS_BASE + 0x2c, alarm >> 32);
    qtest_writel(qts, SNVS_BASE + 0x30, (uint32_t)alarm);
    qtest_writel(qts, SNVS_BASE + 0x08, 0x1 | 0x2);           /* HPTA_EN */
    qtest_clock_step(qts, SEC / 2);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x14) & 1, ==, 0);
    g_assert_false(spi_pending(qts, 19));
    qtest_clock_step(qts, SEC);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x14) & 1, ==, 1);
    g_assert_true(spi_pending(qts, 19));
    /* Disable and clear (W1C) */
    qtest_writel(qts, SNVS_BASE + 0x08, 0x1);
    qtest_writel(qts, SNVS_BASE + 0x14, 0x1);
    g_assert_cmphex(qtest_readl(qts, SNVS_BASE + 0x14) & 1, ==, 0);
    g_assert_false(spi_pending(qts, 19));
    qtest_quit(qts);
}

/* APBH DMA: the module reset sequence of stmp_reset_block() completes */
static void test_apbh_reset(void)
{
    QTestState *qts = qtest_init("-M kick");
    const uint32_t sftrst = 1u << 31, clkgate = 1u << 30;

    g_assert_cmphex(qtest_readl(qts, APBH_BASE), ==, sftrst | clkgate);
    qtest_writel(qts, APBH_BASE + 0x8, sftrst);              /* CLR */
    g_assert_cmphex(qtest_readl(qts, APBH_BASE) & sftrst, ==, 0);
    qtest_writel(qts, APBH_BASE + 0x8, clkgate);
    qtest_writel(qts, APBH_BASE + 0x10 + 0x4, 0xffff0000);   /* CTRL1 SET */
    g_assert_cmphex(qtest_readl(qts, APBH_BASE + 0x10), ==, 0xffff0000);
    qtest_writel(qts, APBH_BASE + 0x4, sftrst);              /* SET */
    /* the block reset and gated its clock */
    g_assert_cmphex(qtest_readl(qts, APBH_BASE), ==, sftrst | clkgate);
    g_assert_cmphex(qtest_readl(qts, APBH_BASE + 0x10), ==, 0);
    qtest_writel(qts, APBH_BASE + 0x8, sftrst);
    qtest_writel(qts, APBH_BASE + 0x8, clkgate);
    g_assert_cmphex(qtest_readl(qts, APBH_BASE), ==, 0);
    /* burst enables as mxs_dma_init sets them, toggle alias */
    qtest_writel(qts, APBH_BASE + 0x4, 3u << 28);
    qtest_writel(qts, APBH_BASE + 0xc, 1u << 28);
    g_assert_cmphex(qtest_readl(qts, APBH_BASE), ==, 1u << 29);
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

/* uSDHC registers and command flags (SDHCI command register, upper half) */
#define USDHC_ARG2      0x00
#define USDHC_BLK_ATT   0x04
#define USDHC_CMD_ARG   0x08
#define USDHC_XFER_CMD  0x0c
#define USDHC_RSP0      0x10
#define USDHC_DATA      0x20
#define USDHC_SYS_CTRL  0x2c
#define USDHC_INT_STS   0x30
#define USDHC_MIX_CTRL  0x48
#define RSP_136         0x01
#define RSP_48          0x02
#define RSP_48B         0x03
#define RSP_CRC         0x08
#define RSP_IDX         0x10
#define CMD_DATA        0x20
#define MIX_BCEN        0x02
#define MIX_READ        0x10
#define MIX_MULTI       0x20
#define MIX_AC23EN      0x80
#define EMMC_RCA        0x00010000
#define EMMC_BLOCKS     2
#define EMMC_SIZE       (4 * 1024 * 1024)

static uint32_t usdhc_cmd(QTestState *qts, int index, uint32_t arg,
                          uint32_t flags)
{
    qtest_writel(qts, USDHC3_BASE + USDHC_CMD_ARG, arg);
    qtest_writel(qts, USDHC3_BASE + USDHC_XFER_CMD,
                 ((index << 8) | flags) << 16);
    qtest_writel(qts, USDHC3_BASE + USDHC_INT_STS, 0x1);   /* CC */
    return qtest_readl(qts, USDHC3_BASE + USDHC_RSP0);
}

/* CURRENT_STATE of the card status (R1), 4 = tran */
static int emmc_state(QTestState *qts)
{
    uint32_t r1 = usdhc_cmd(qts, 13, EMMC_RCA, RSP_48 | RSP_CRC | RSP_IDX);

    return (r1 >> 9) & 0xf;
}

/*
 * Linux sends multi-block eMMC transfers with Auto CMD23 (MIX_CTRL.AC23EN,
 * block count in DS_ADDR) and no CMD12: after the last block the card must
 * be back in the transfer state on its own. Also checks the eMMC erase.
 */
static void test_usdhc_auto_cmd23(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    uint8_t image[EMMC_BLOCKS * 512];
    QTestState *qts;
    int fd, i;

    fd = g_file_open_tmp("kick-emmc-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, EMMC_SIZE), ==, 0);
    for (i = 0; i < sizeof(image); i++) {
        image[i] = i * 7 + 3;
    }
    g_assert_cmpint(pwrite(fd, image, sizeof(image), 0), ==, sizeof(image));
    args = g_strdup_printf("-M kick,emmc=emmc "
                           "-drive if=none,id=emmc,format=raw,file=%s", path);
    qts = qtest_init(args);

    /* The card clock runs once SYS_CTRL is written (as every driver does) */
    qtest_writel(qts, USDHC3_BASE + USDHC_SYS_CTRL,
                 qtest_readl(qts, USDHC3_BASE + USDHC_SYS_CTRL));

    /* Identification: CMD0, CMD1 until powered up, CMD2, CMD3, CMD7 */
    usdhc_cmd(qts, 0, 0, 0);
    for (i = 0; i < 10; i++) {
        if (usdhc_cmd(qts, 1, 0x40ff8080, RSP_48) & 0x80000000) {
            break;
        }
        qtest_clock_step(qts, 1000000);
    }
    g_assert_cmpint(i, <, 10);
    usdhc_cmd(qts, 2, 0, RSP_136 | RSP_CRC);
    usdhc_cmd(qts, 3, EMMC_RCA, RSP_48 | RSP_CRC | RSP_IDX);
    usdhc_cmd(qts, 7, EMMC_RCA, RSP_48B | RSP_CRC | RSP_IDX);
    g_assert_cmpint(emmc_state(qts), ==, 4);

    /*
     * CMD18 with Auto CMD23, PIO. ARGUMENT2 shares its register with the
     * SDMA address (Linux uses Auto CMD23 with ADMA only), so it is written
     * while the block count is still 0 and no SDMA transfer can start.
     */
    qtest_writel(qts, USDHC3_BASE + USDHC_MIX_CTRL,
                 MIX_AC23EN | MIX_MULTI | MIX_READ | MIX_BCEN);
    qtest_writel(qts, USDHC3_BASE + USDHC_ARG2, EMMC_BLOCKS);
    qtest_writel(qts, USDHC3_BASE + USDHC_BLK_ATT, (EMMC_BLOCKS << 16) | 512);
    usdhc_cmd(qts, 18, 0, RSP_48 | RSP_CRC | RSP_IDX | CMD_DATA);
    for (i = 0; i < sizeof(image); i += 4) {
        g_assert_cmphex(qtest_readl(qts, USDHC3_BASE + USDHC_DATA), ==,
                        (uint32_t)ldl_le_p(&image[i]));
    }
    g_assert_cmpint(emmc_state(qts), ==, 4);

    /* CMD25 with Auto CMD23 (reliable write flag in the argument, as Linux) */
    qtest_writel(qts, USDHC3_BASE + USDHC_MIX_CTRL,
                 MIX_AC23EN | MIX_MULTI | MIX_BCEN);
    qtest_writel(qts, USDHC3_BASE + USDHC_ARG2, 0x80000000 | EMMC_BLOCKS);
    qtest_writel(qts, USDHC3_BASE + USDHC_BLK_ATT, (EMMC_BLOCKS << 16) | 512);
    usdhc_cmd(qts, 25, 0, RSP_48 | RSP_CRC | RSP_IDX | CMD_DATA);
    for (i = 0; i < sizeof(image); i += 4) {
        qtest_writel(qts, USDHC3_BASE + USDHC_DATA,
                     ~(uint32_t)ldl_le_p(&image[i]));
    }
    g_assert_cmpint(emmc_state(qts), ==, 4);
    g_assert_cmpint(pread(fd, image, sizeof(image), 0), ==, sizeof(image));
    g_assert_cmphex(image[0], ==, (uint8_t)~3);
    g_assert_cmphex(image[sizeof(image) - 1], ==,
                    (uint8_t)~((sizeof(image) - 1) * 7 + 3));

    /*
     * Erase (CMD35/36/38) of the whole card, as Linux discards a device
     * before mkfs: EXT_CSD ERASED_MEM_CONT is 0, so it reads back as zeros.
     */
    usdhc_cmd(qts, 35, 0, RSP_48 | RSP_CRC | RSP_IDX);
    usdhc_cmd(qts, 36, EMMC_SIZE / 512 - 1, RSP_48 | RSP_CRC | RSP_IDX);
    usdhc_cmd(qts, 38, 0, RSP_48B | RSP_CRC | RSP_IDX);
    g_assert_cmpint(emmc_state(qts), ==, 4);
    qtest_quit(qts);

    g_assert_cmpint(pread(fd, image, sizeof(image), 0), ==, sizeof(image));
    for (i = 0; i < sizeof(image); i++) {
        g_assert_cmphex(image[i], ==, 0);
    }
    close(fd);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/src", test_src);
    qtest_add_func("/kick/gpc", test_gpc);
    qtest_add_func("/kick/iomuxc", test_iomuxc);
    qtest_add_func("/kick/tmu", test_tmu);
    qtest_add_func("/kick/snvs", test_snvs);
    qtest_add_func("/kick/snvs-long-press", test_snvs_long_press);
    qtest_add_func("/kick/snvs-hp-alarm", test_snvs_hp_alarm);
    qtest_add_func("/kick/tmu-irq", test_tmu_irq);
    qtest_add_func("/kick/gpio1-pin-irq", test_gpio1_pin_irq);
    qtest_add_func("/kick/apbh-dma-reset", test_apbh_reset);
    qtest_add_func("/kick/sysctr", test_sysctr);
    qtest_add_func("/kick/cqhci", test_cqhci);
    qtest_add_func("/kick/usdhc-auto-cmd23", test_usdhc_auto_cmd23);

    return g_test_run();
}
