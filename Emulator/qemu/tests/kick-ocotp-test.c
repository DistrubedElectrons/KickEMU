/*
 * qtest for the i.MX8MM OCOTP model
 *
 * Checks the fuse shadows the SoC identification code reads (unique ID from
 * the machine property, variant word for an i.MX8MM Quad), the CTRL
 * SET/CLR aliases, fuse programming with the unlock key, RELOAD_SHADOWS,
 * direct fuse reads and the error bit for a locked write.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define OCOTP_BASE          0x30350000
#define CTRL                0x000
#define CTRL_SET            0x004
#define CTRL_CLR            0x008
#define DATA                0x020
#define READ_CTRL           0x030
#define READ_FUSE_DATA      0x040
#define SHADOW(n)           (0x400 + (n) * 0x10)

#define CTRL_BUSY           (1u << 8)
#define CTRL_ERROR          (1u << 9)
#define CTRL_RELOAD         (1u << 10)
#define CTRL_UNLOCK         0x3e770000

#define UID                 0x0123456789abcdefULL

static uint32_t rd(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, OCOTP_BASE + off);
}

static void wr(QTestState *qts, uint32_t off, uint32_t v)
{
    qtest_writel(qts, OCOTP_BASE + off, v);
}

static void test_soc_id(void)
{
    QTestState *qts = qtest_initf("-M kick,soc-uid=0x%" PRIx64, UID);

    /* soc-imx8m.c: UID = HW_OCOTP 0x420 << 32 | 0x410 */
    g_assert_cmphex(rd(qts, 0x410), ==, (uint32_t)UID);
    g_assert_cmphex(rd(qts, 0x420), ==, (uint32_t)(UID >> 32));
    /* TESTER4 (0x450) variant bits 0: full i.MX8MM Quad in U-Boot */
    g_assert_cmphex(rd(qts, 0x450), ==, 0);
    g_assert_cmphex(rd(qts, CTRL), ==, 0);
    qtest_quit(qts);
}

static void test_mac_fuses(void)
{
    QTestState *qts = qtest_init("-M kick -nic user,mac=00:04:9f:01:02:03");

    /* U-Boot imx_get_mac_from_fuse: word 0x640 = mac[2..5], 0x650 = mac[0..1] */
    g_assert_cmphex(rd(qts, SHADOW(36)), ==, 0x9f010203);
    g_assert_cmphex(rd(qts, SHADOW(37)), ==, 0x00000004);
    qtest_quit(qts);
}

static void test_program(void)
{
    QTestState *qts = qtest_init("-M kick");
    const int word = 0x30;  /* a general purpose fuse word */

    /* Program without the unlock key: ERROR, fuse unchanged */
    wr(qts, CTRL, word);
    wr(qts, DATA, 0x00f0);
    g_assert_cmphex(rd(qts, CTRL) & CTRL_ERROR, ==, CTRL_ERROR);
    wr(qts, CTRL_CLR, CTRL_ERROR);
    g_assert_cmphex(rd(qts, CTRL) & (CTRL_ERROR | CTRL_BUSY), ==, 0);

    /* Program with the key; the key clears itself after the cycle */
    wr(qts, CTRL, CTRL_UNLOCK | word);
    wr(qts, DATA, 0x00f0);
    g_assert_cmphex(rd(qts, CTRL), ==, word);
    /* The shadow keeps the old value until a reload */
    g_assert_cmphex(rd(qts, SHADOW(word)), ==, 0);
    wr(qts, CTRL_SET, CTRL_RELOAD);
    g_assert_cmphex(rd(qts, CTRL) & CTRL_RELOAD, ==, 0);
    g_assert_cmphex(rd(qts, SHADOW(word)), ==, 0x00f0);

    /* Fuses only blow 0 -> 1 */
    wr(qts, CTRL, CTRL_UNLOCK | word);
    wr(qts, DATA, 0x000f);
    wr(qts, READ_CTRL, 1);
    g_assert_cmphex(rd(qts, READ_CTRL), ==, 0);
    g_assert_cmphex(rd(qts, READ_FUSE_DATA), ==, 0x00ff);

    /* Shadow registers can be overridden by software */
    wr(qts, SHADOW(word), 0x1234);
    g_assert_cmphex(rd(qts, SHADOW(word)), ==, 0x1234);
    wr(qts, CTRL_SET, CTRL_RELOAD);
    g_assert_cmphex(rd(qts, SHADOW(word)), ==, 0x00ff);

    /* Fuses survive a system reset, the shadows reload from them */
    qtest_system_reset(qts);
    g_assert_cmphex(rd(qts, SHADOW(word)), ==, 0x00ff);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/ocotp/soc-id", test_soc_id);
    qtest_add_func("/kick/ocotp/mac-fuses", test_mac_fuses);
    qtest_add_func("/kick/ocotp/program", test_program);

    return g_test_run();
}
