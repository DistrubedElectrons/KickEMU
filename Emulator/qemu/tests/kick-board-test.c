/*
 * qtest for the kick board devices: BD71847 PMIC on I2C1 (through the
 * i.MX I2C controller), AR8033 PHY on the ENET MDIO bus, and a smoke test
 * of the machine (memory map, UART, unimplemented windows).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define I2C1_BASE       0x30a20000
#define ENET_BASE       0x30be0000
#define UART4_BASE      0x30a60000
#define OCRAM_BASE      0x00900000
#define RAM_BASE        0x40000000

/* i.MX I2C registers (16-bit, 4-byte stride) */
#define I2DR            0x10
#define I2CR            0x08
#define I2SR            0x0c
#define I2CR_IEN        0x80
#define I2CR_MSTA       0x20
#define I2CR_MTX        0x10
#define I2CR_TXAK       0x08
#define I2SR_IBB        0x20
#define I2SR_IIF        0x02
#define I2SR_RXAK       0x01

#define PMIC_ADDR       0x4b

static void i2c_wait_byte(QTestState *qts)
{
    uint8_t sr = qtest_readb(qts, I2C1_BASE + I2SR);

    g_assert(sr & I2SR_IIF);
    g_assert_false(sr & I2SR_RXAK);
    qtest_writeb(qts, I2C1_BASE + I2SR, 0);
}

static void i2c_write(QTestState *qts, uint8_t addr, const uint8_t *buf,
                      int len)
{
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN | I2CR_MSTA | I2CR_MTX);
    g_assert(qtest_readb(qts, I2C1_BASE + I2SR) & I2SR_IBB);
    qtest_writeb(qts, I2C1_BASE + I2DR, addr << 1);
    i2c_wait_byte(qts);
    for (int i = 0; i < len; i++) {
        qtest_writeb(qts, I2C1_BASE + I2DR, buf[i]);
        i2c_wait_byte(qts);
    }
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN);
}

static uint8_t pmic_read(QTestState *qts, uint8_t reg)
{
    uint8_t v;

    i2c_write(qts, PMIC_ADDR, &reg, 1);
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN | I2CR_MSTA | I2CR_MTX);
    qtest_writeb(qts, I2C1_BASE + I2DR, (PMIC_ADDR << 1) | 1);
    i2c_wait_byte(qts);
    /* single byte: no ACK, dummy read starts the transfer */
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN | I2CR_MSTA | I2CR_TXAK);
    qtest_readb(qts, I2C1_BASE + I2DR);
    i2c_wait_byte(qts);
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN | I2CR_TXAK);
    v = qtest_readb(qts, I2C1_BASE + I2DR);
    return v;
}

static void pmic_write(QTestState *qts, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    i2c_write(qts, PMIC_ADDR, buf, 2);
}

static void test_pmic(void)
{
    QTestState *qts = qtest_init("-M kick");

    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN);
    /* Locked after reset: PWRSEQ | VREG */
    g_assert_cmphex(pmic_read(qts, 0x2f), ==, 0x11);
    g_assert_cmphex(pmic_read(qts, 0x10), ==, 0x1e);   /* BUCK2 1.00 V */
    g_assert_cmphex(pmic_read(qts, 0x18), ==, 0x62);   /* LDO1 1.8 V */

    /* Writes to regulator registers are ignored while VREG is locked */
    pmic_write(qts, 0x10, 0x14);
    g_assert_cmphex(pmic_read(qts, 0x10), ==, 0x1e);
    /* SPL unlocks VREG (REGLOCK = 0x01) */
    pmic_write(qts, 0x2f, 0x01);
    pmic_write(qts, 0x10, 0x14);
    g_assert_cmphex(pmic_read(qts, 0x10), ==, 0x14);
    /* Status registers are read-only */
    pmic_write(qts, 0x2d, 0x00);
    g_assert_cmphex(pmic_read(qts, 0x2d), ==, 0x08);
    qtest_quit(qts);
}

static void test_i2c_nack(void)
{
    QTestState *qts = qtest_init("-M kick");
    uint8_t sr;

    /* No device at 0x50: the byte still completes (IIF) with RXAK set */
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN);
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN | I2CR_MSTA | I2CR_MTX);
    qtest_writeb(qts, I2C1_BASE + I2DR, 0x50 << 1);
    sr = qtest_readb(qts, I2C1_BASE + I2SR);
    g_assert_cmphex(sr & (I2SR_IIF | I2SR_RXAK), ==, I2SR_IIF | I2SR_RXAK);
    qtest_writeb(qts, I2C1_BASE + I2SR, 0);
    qtest_writeb(qts, I2C1_BASE + I2CR, I2CR_IEN);
    g_assert_cmphex(qtest_readb(qts, I2C1_BASE + I2SR) & I2SR_IBB, ==, 0);
    /* The PMIC still answers afterwards */
    g_assert_cmphex(pmic_read(qts, 0x2f), ==, 0x11);
    qtest_quit(qts);
}

static uint16_t mdio(QTestState *qts, int op, int reg, uint16_t data)
{
    /* clause 22: ST 01, OP, PA 4, RA, TA 10 */
    uint32_t frame = (1u << 30) | ((uint32_t)op << 28) | (4u << 23) |
                     ((uint32_t)reg << 18) | (2u << 16) | data;

    qtest_writel(qts, ENET_BASE + 0x04, 1u << 23);  /* clear EIR.MII */
    qtest_writel(qts, ENET_BASE + 0x40, frame);
    /* The MAC reports completion of every management frame */
    g_assert_cmphex(qtest_readl(qts, ENET_BASE + 0x04) & (1u << 23), !=, 0);
    return qtest_readl(qts, ENET_BASE + 0x40) & 0xffff;
}

static void test_phy(void)
{
    QTestState *qts = qtest_init("-M kick -nic user");

    /* AR8033 PHY ID */
    g_assert_cmphex(mdio(qts, 2, 2, 0), ==, 0x004d);
    g_assert_cmphex(mdio(qts, 2, 3, 0), ==, 0xd074);
    /* Link up, autonegotiation complete, 1000 full duplex resolved */
    g_assert_cmphex(mdio(qts, 2, 1, 0) & 0x24, ==, 0x24);
    g_assert_cmphex(mdio(qts, 2, 0x11, 0), ==, 0xac00);
    /* Debug port */
    mdio(qts, 1, 0x1d, 0x05);
    mdio(qts, 1, 0x1e, 0x1234);
    mdio(qts, 1, 0x1d, 0x05);
    g_assert_cmphex(mdio(qts, 2, 0x1e, 0), ==, 0x1234);
    /* Soft reset restores the defaults */
    mdio(qts, 1, 0, 0x8000);
    g_assert_cmphex(mdio(qts, 2, 0, 0), ==, 0x1140);
    /* Nothing at another address: the bus reads all ones */
    qtest_writel(qts, ENET_BASE + 0x40, (1u << 30) | (2u << 28) | (5u << 23) |
                 (2u << 16));
    g_assert_cmphex(qtest_readl(qts, ENET_BASE + 0x40) & 0xffff, ==, 0xffff);
    qtest_quit(qts);
}

static void test_machine(void)
{
    QTestState *qts = qtest_init("-M kick -m 1G");

    /* On-chip RAM and DDR are memory */
    qtest_writel(qts, OCRAM_BASE, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, OCRAM_BASE), ==, 0xdeadbeef);
    qtest_writel(qts, RAM_BASE + 0x40000000ULL - 4, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, RAM_BASE + 0x40000000ULL - 4),
                    ==, 0x12345678);
    /* UART4: UTS reset value (TXEMPTY | RXEMPTY) */
    g_assert_cmphex(qtest_readl(qts, UART4_BASE + 0xb4) & 0x60, ==, 0x60);
    /* An unimplemented block reads as zero instead of faulting */
    g_assert_cmphex(qtest_readl(qts, 0x30900000), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/board/pmic", test_pmic);
    qtest_add_func("/kick/board/i2c-nack", test_i2c_nack);
    qtest_add_func("/kick/board/enet-phy", test_phy);
    qtest_add_func("/kick/board/machine", test_machine);

    return g_test_run();
}
