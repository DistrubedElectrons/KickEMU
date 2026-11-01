/*
 * qtest for the i.MX8MM MIPI DSIM and the SN65DSI84 bridge models
 *
 * Replays the register sequence of sec-dsim.c (bind, bridge enable, packet
 * transfer) and of the sn65dsi83 bridge driver (probe, configure, start
 * stream) on the kick-display-bench machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/i2c.h"

#define DSIM_BASE           0x32e10000
#define I2C1_BASE           0x30a20000
#define SN65_ADDR           0x2c

#define DSIM_VERSION        0x00
#define DSIM_STATUS         0x04
#define DSIM_CLKCTRL        0x10
#define DSIM_CONFIG         0x18
#define DSIM_MDRESOL        0x20
#define DSIM_INTSRC         0x34
#define DSIM_INTMSK         0x38
#define DSIM_PKTHDR         0x3c
#define DSIM_PAYLOAD        0x40
#define DSIM_RXFIFO         0x44
#define DSIM_FIFOCTRL       0x4c
#define DSIM_PLLCTRL        0x94
#define DSIM_PLLTMR         0xa0

#define STATUS_PLLSTABLE    (1u << 31)
#define STATUS_TXREADYHSCLK (1u << 10)
#define STATUS_STOPSTATECLK (1u << 8)
#define INTSRC_PLLSTABLE    (1u << 31)
#define INTSRC_SWRSTRELEASE (1u << 30)
#define INTSRC_SFRPLFIFOEMPTY (1u << 29)
#define INTSRC_SFRPHFIFOEMPTY (1u << 28)
#define INTSRC_TATOUT       (1u << 20)
#define INTSRC_RXDATDONE    (1u << 18)
#define FIFOCTRL_EMPTYRX    (1u << 24)
#define FIFOCTRL_EMPTYLSFR  (1u << 20)
#define PLLCTRL_PLLEN       (1u << 23)

static uint32_t dsim_rd(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, DSIM_BASE + off);
}

static void dsim_wr(QTestState *qts, uint32_t off, uint32_t v)
{
    qtest_writel(qts, DSIM_BASE + off, v);
}

static QTestState *start(void)
{
    return qtest_init("-machine kick-display-bench -display none");
}

/* sec_mipi_dsim_irq_unmask() for the given INTMSK bits. */
static void dsim_unmask(QTestState *qts, uint32_t bits)
{
    dsim_wr(qts, DSIM_INTMSK, dsim_rd(qts, DSIM_INTMSK) & ~bits);
}

/*
 * The DSIM part of sec_mipi_dsim_bridge_enable() for four lanes, RGB888
 * burst video mode: main mode, DPI config, PLL, FIFO init, clock control.
 */
static void dsim_enable(QTestState *qts)
{
    uint32_t pms = (2 << 13) | (110 << 3) | 2;

    dsim_wr(qts, DSIM_MDRESOL, (600 << 16) | 1024);
    dsim_wr(qts, DSIM_CONFIG, (1u << 25) | (1u << 26) | (7 << 12) |
                              (3 << 5) | 0x1f);
    dsim_wr(qts, DSIM_PLLTMR, 0x8000);
    dsim_wr(qts, DSIM_PLLCTRL, (pms << 1) | PLLCTRL_PLLEN);
}

static void test_dsim_reset_values(void)
{
    QTestState *qts = start();

    g_assert_cmphex(dsim_rd(qts, DSIM_VERSION), ==, 0x01060200);
    g_assert_cmphex(dsim_rd(qts, DSIM_INTMSK), ==, 0xb337ffff);
    g_assert_cmphex(dsim_rd(qts, DSIM_INTSRC), ==, 0);
    g_assert_cmphex(dsim_rd(qts, DSIM_FIFOCTRL) & 0x1f, ==, 0x1f);
    g_assert_cmphex(dsim_rd(qts, DSIM_FIFOCTRL) &
                    (FIFOCTRL_EMPTYRX | FIFOCTRL_EMPTYLSFR), ==,
                    FIFOCTRL_EMPTYRX | FIFOCTRL_EMPTYLSFR);
    g_assert_cmphex(dsim_rd(qts, DSIM_STATUS) & STATUS_PLLSTABLE, ==, 0);
    qtest_quit(qts);
}

static void test_dsim_init_sequence(void)
{
    QTestState *qts = start();
    uint32_t status, fifoctrl;

    qtest_irq_intercept_out_named(qts, "/machine/dsim", "sysbus-irq");

    /* sec_mipi_dsim_irq_init() with a bridge attached. */
    dsim_unmask(qts, INTSRC_PLLSTABLE | INTSRC_SWRSTRELEASE);
    g_assert_false(qtest_get_irq(qts, 0));

    dsim_enable(qts);
    g_assert_cmphex(dsim_rd(qts, DSIM_STATUS) & STATUS_PLLSTABLE, ==, 0);

    /* 0x8000 cycles of the 12 MHz reference: about 2.7 ms. */
    qtest_clock_step(qts, 2 * 1000 * 1000);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1 * 1000 * 1000);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(dsim_rd(qts, DSIM_INTSRC), ==, INTSRC_PLLSTABLE);

    /* The ISR clears the source by writing one. */
    dsim_wr(qts, DSIM_INTSRC, INTSRC_PLLSTABLE);
    g_assert_false(qtest_get_irq(qts, 0));

    /* sec_mipi_dsim_config_pll(): clock and all four data lanes stopped. */
    status = dsim_rd(qts, DSIM_STATUS);
    g_assert_cmphex(status & STATUS_PLLSTABLE, ==, STATUS_PLLSTABLE);
    g_assert_cmphex(status & STATUS_STOPSTATECLK, ==, STATUS_STOPSTATECLK);
    g_assert_cmphex(status & 0xf, ==, 0xf);

    /* sec_mipi_dsim_init_fifo_pointers() */
    fifoctrl = dsim_rd(qts, DSIM_FIFOCTRL);
    dsim_wr(qts, DSIM_FIFOCTRL, fifoctrl & ~0x1f);
    dsim_wr(qts, DSIM_FIFOCTRL, fifoctrl | 0x1f);
    g_assert_cmphex(dsim_rd(qts, DSIM_FIFOCTRL) & 0x1f, ==, 0x1f);

    /* sec_mipi_dsim_config_clkctrl(): HS clock requested. */
    dsim_wr(qts, DSIM_CLKCTRL, (1u << 31) | (1u << 28) | (1u << 24) |
                               (0x1f << 19) | 3);
    status = dsim_rd(qts, DSIM_STATUS);
    g_assert_cmphex(status & STATUS_TXREADYHSCLK, ==, STATUS_TXREADYHSCLK);
    g_assert_cmphex(status & STATUS_STOPSTATECLK, ==, 0);

    /* sec_mipi_dsim_set_standby(): video streams, data lanes leave LP-11. */
    dsim_wr(qts, DSIM_MDRESOL, dsim_rd(qts, DSIM_MDRESOL) | (1u << 31));
    g_assert_cmphex(dsim_rd(qts, DSIM_STATUS) & 0xf, ==, 0);

    /* Disabling the PLL drops the lock. */
    dsim_wr(qts, DSIM_PLLCTRL, dsim_rd(qts, DSIM_PLLCTRL) & ~PLLCTRL_PLLEN);
    g_assert_cmphex(dsim_rd(qts, DSIM_STATUS) & STATUS_PLLSTABLE, ==, 0);
    qtest_quit(qts);
}

static void test_dsim_packets(void)
{
    QTestState *qts = start();

    qtest_irq_intercept_out_named(qts, "/machine/dsim", "sysbus-irq");
    dsim_enable(qts);
    qtest_clock_step(qts, 5 * 1000 * 1000);
    dsim_wr(qts, DSIM_INTSRC, ~0u);
    dsim_unmask(qts, INTSRC_SFRPLFIFOEMPTY | INTSRC_SFRPHFIFOEMPTY |
                     INTSRC_RXDATDONE | INTSRC_TATOUT);

    /* DCS long write of 5 bytes: two payload words, then the header. */
    dsim_wr(qts, DSIM_PAYLOAD, 0x0403022a);
    g_assert_cmphex(dsim_rd(qts, DSIM_FIFOCTRL) & FIFOCTRL_EMPTYLSFR, ==, 0);
    dsim_wr(qts, DSIM_PAYLOAD, 0x00000005);
    dsim_wr(qts, DSIM_PKTHDR, (0 << 16) | (5 << 8) | 0x39);
    g_assert_cmphex(dsim_rd(qts, DSIM_INTSRC), ==,
                    INTSRC_SFRPHFIFOEMPTY | INTSRC_SFRPLFIFOEMPTY);
    g_assert_cmphex(dsim_rd(qts, DSIM_FIFOCTRL) & FIFOCTRL_EMPTYLSFR, ==,
                    FIFOCTRL_EMPTYLSFR);
    g_assert_true(qtest_get_irq(qts, 0));
    dsim_wr(qts, DSIM_INTSRC, INTSRC_SFRPHFIFOEMPTY | INTSRC_SFRPLFIFOEMPTY);
    g_assert_false(qtest_get_irq(qts, 0));

    /* DCS short write without parameter. */
    dsim_wr(qts, DSIM_PKTHDR, (0x29 << 8) | 0x05);
    g_assert_cmphex(dsim_rd(qts, DSIM_INTSRC) & INTSRC_SFRPHFIFOEMPTY, ==,
                    INTSRC_SFRPHFIFOEMPTY);
    dsim_wr(qts, DSIM_INTSRC, ~0u);

    /*
     * DCS read (get_power_mode). Nothing on the KICK answers DSI reads (the
     * SN65DSI84 is configured over I2C), so the turnaround times out and the
     * RX FIFO stays empty.
     */
    dsim_wr(qts, DSIM_PKTHDR, (0x0a << 8) | 0x06);
    g_assert_cmphex(dsim_rd(qts, DSIM_INTSRC) & (INTSRC_TATOUT |
                    INTSRC_RXDATDONE), ==, INTSRC_TATOUT);
    g_assert_cmphex(dsim_rd(qts, DSIM_FIFOCTRL) & FIFOCTRL_EMPTYRX, ==,
                    FIFOCTRL_EMPTYRX);
    qtest_quit(qts);
}

/* Raw access to the i.MX I2C controller to see the address NACK. */
#define I2C_IADR            0x00
#define I2C_I2CR            0x08
#define I2C_I2SR            0x0c
#define I2C_I2DR            0x10
#define I2CR_IEN            0x80
#define I2CR_MSTA           0x20
#define I2CR_MTX            0x10
#define I2SR_RXAK           0x01

static bool sn65_acks(QTestState *qts)
{
    uint8_t status;

    qtest_writeb(qts, I2C1_BASE + I2C_I2CR, I2CR_IEN | I2CR_MSTA | I2CR_MTX);
    qtest_writeb(qts, I2C1_BASE + I2C_I2DR, SN65_ADDR << 1);
    status = qtest_readb(qts, I2C1_BASE + I2C_I2SR);
    qtest_writeb(qts, I2C1_BASE + I2C_I2SR, 0);
    qtest_writeb(qts, I2C1_BASE + I2C_I2CR, I2CR_IEN);
    return !(status & I2SR_RXAK);
}

static void test_sn65dsi83(void)
{
    QTestState *qts = start();
    IMXI2C i2c;
    QI2CDevice dev = { .addr = SN65_ADDR };
    uint8_t id[9];
    static const uint8_t want_id[9] = {
        0x35, 0x38, 0x49, 0x53, 0x44, 0x20, 0x20, 0x20, 0x01
    };

    imx_i2c_init(&i2c, qts, I2C1_BASE);
    dev.bus = &i2c.parent;

    /* EN low: the bridge is held in reset and does not answer. */
    g_assert_false(sn65_acks(qts));

    /* sn65dsi83_brg_power_on(): EN high. */
    qtest_set_irq_in(qts, "/machine/sn65dsi83", "enable", 0, 1);
    g_assert_true(sn65_acks(qts));

    /* Probe: sn65dsi83_brg_reset() expects SOFT_RESET to read 0. */
    g_assert_cmphex(i2c_get8(&dev, 0x09), ==, 0);
    i2c_read_block(&dev, 0x00, id, sizeof(id));
    g_assert_cmpmem(id, sizeof(id), want_id, sizeof(want_id));

    /* sn65dsi83_brg_configure() for 4 lanes, 1024x600, DSI clock source. */
    i2c_set8(&dev, 0x09, 0x00);
    i2c_set8(&dev, 0x0d, 0x00);
    i2c_set8(&dev, 0x0a, 0x01);
    i2c_set8(&dev, 0x12, 0x0b);
    i2c_set8(&dev, 0x0b, 0x28);
    g_assert_cmphex(i2c_get8(&dev, 0x10), ==, 0x26);
    i2c_set8(&dev, 0x10, 0x26 & ~(3 << 3));
    i2c_set8(&dev, 0x20, 1024 & 0xff);
    i2c_set8(&dev, 0x21, 1024 >> 8);
    g_assert_cmphex(i2c_get8(&dev, 0x20), ==, 0x00);
    g_assert_cmphex(i2c_get8(&dev, 0x21), ==, 0x04);

    /* PLL_EN without a running DSI HS clock: no lock, PLL_UNLOCK latched. */
    i2c_set8(&dev, 0x0d, 0x01);
    g_assert_cmphex(i2c_get8(&dev, 0x0a) & 0x80, ==, 0);
    g_assert_cmphex(i2c_get8(&dev, 0xe5) & 0x01, ==, 0x01);

    /* Start the DSIM HS clock: the bridge PLL locks. */
    dsim_enable(qts);
    qtest_clock_step(qts, 5 * 1000 * 1000);
    dsim_wr(qts, DSIM_CLKCTRL, (1u << 31) | (1u << 28) | (1u << 24));
    g_assert_cmphex(i2c_get8(&dev, 0x0a), ==, 0x81);

    /* sn65dsi83_brg_start_stream(): soft reset, then clear errors (W1C). */
    i2c_set8(&dev, 0x09, 0x01);
    g_assert_cmphex(i2c_get8(&dev, 0x09), ==, 0);
    i2c_set8(&dev, 0xe5, 0xff);
    g_assert_cmphex(i2c_get8(&dev, 0xe5), ==, 0);

    /* EN low again resets the register file. */
    qtest_set_irq_in(qts, "/machine/sn65dsi83", "enable", 0, 0);
    g_assert_false(sn65_acks(qts));
    qtest_set_irq_in(qts, "/machine/sn65dsi83", "enable", 0, 1);
    g_assert_cmphex(i2c_get8(&dev, 0x20), ==, 0);
    g_assert_cmphex(i2c_get8(&dev, 0x0a), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/dsim/reset_values", test_dsim_reset_values);
    qtest_add_func("/kick/dsim/init_sequence", test_dsim_init_sequence);
    qtest_add_func("/kick/dsim/packets", test_dsim_packets);
    qtest_add_func("/kick/dsim/sn65dsi83", test_sn65dsi83);

    return g_test_run();
}
