/*
 * qtest for the USB controllers of the kick machine: ChipIdea EHCI in host
 * role at USB1 (bus usb-c.0) and USB2 (bus usb-a.0), the integrated TT of
 * the root port (full/low-speed enable, PORTSC.PSPD) and usbmisc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define USB1_BASE           0x32e40000
#define USB2_BASE           0x32e50000
#define CAPLENGTH           0x100
#define HCSPARAMS           0x104
#define DCCPARAMS           0x124
#define USBCMD              0x140
#define PORTSC1             0x184
#define USBMISC_CTRL1       0x200
#define USBMISC_CTRL2       0x204
#define USBMISC_PHY_STATUS  0x23c

#define PORTSC_CONNECT      (1u << 0)
#define PORTSC_PED          (1u << 2)
#define PORTSC_PRESET       (1u << 8)
#define PORTSC_PP           (1u << 12)
#define PORTSC_PSPD(v)      (((v) >> 26) & 3)

static void test_registers(void)
{
    QTestState *qts = qtest_init("-M kick");
    static const uint32_t base[] = { USB1_BASE, USB2_BASE };

    for (int i = 0; i < 2; i++) {
        /* EHCI capabilities: operational registers at +0x40, one port */
        g_assert_cmphex(qtest_readl(qts, base[i] + CAPLENGTH) & 0xff, ==,
                        0x40);
        g_assert_cmphex(qtest_readl(qts, base[i] + HCSPARAMS) & 0xf, ==, 1);
        /* host capable, no device controller: ci_hdrc runs host only */
        g_assert_cmphex(qtest_readl(qts, base[i] + DCCPARAMS), ==, 0x100);
        /* usbmisc: power-on value usbmisc_imx7d_power_lost_check expects */
        g_assert_cmphex(qtest_readl(qts, base[i] + USBMISC_CTRL1), ==,
                        0x30001000);
        qtest_writel(qts, base[i] + USBMISC_CTRL2, 0x00000006);
        g_assert_cmphex(qtest_readl(qts, base[i] + USBMISC_CTRL2), ==, 6);
        qtest_writel(qts, base[i] + USBMISC_PHY_STATUS, 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, base[i] + USBMISC_PHY_STATUS), ==, 0);
    }
    /* The two usbmisc blocks are separate */
    qtest_writel(qts, USB1_BASE + USBMISC_CTRL1, 0x30001080);
    g_assert_cmphex(qtest_readl(qts, USB2_BASE + USBMISC_CTRL1), ==,
                    0x30001000);
    /* and reset with the machine */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, USB1_BASE + USBMISC_CTRL1), ==,
                    0x30001000);
    qtest_quit(qts);
}

/* Reset the root port as ehci-hcd does and return PORTSC1 afterwards */
static uint32_t port_reset(QTestState *qts, uint32_t base)
{
    uint32_t v = qtest_readl(qts, base + PORTSC1);

    g_assert_cmphex(v & PORTSC_CONNECT, ==, PORTSC_CONNECT);
    qtest_writel(qts, base + PORTSC1, (v & ~PORTSC_PED) | PORTSC_PP |
                 PORTSC_PRESET);
    qtest_writel(qts, base + PORTSC1, PORTSC_PP);
    return qtest_readl(qts, base + PORTSC1);
}

/* Full speed device on the root port: enabled through the integrated TT */
static void test_full_speed_port(void)
{
    QTestState *qts = qtest_init("-M kick -device usb-kbd,bus=usb-a.0,"
                                 "port=1,usb_version=1");
    uint32_t v = port_reset(qts, USB2_BASE);

    g_assert_cmphex(v & PORTSC_PED, ==, PORTSC_PED);
    g_assert_cmpuint(PORTSC_PSPD(v), ==, 0);
    /* nothing on the other controller */
    g_assert_cmphex(qtest_readl(qts, USB1_BASE + PORTSC1) & PORTSC_CONNECT,
                    ==, 0);
    qtest_quit(qts);
}

/* High speed device attached directly: PSPD reports high speed */
static void test_high_speed_port(void)
{
    QTestState *qts = qtest_init("-M kick -drive if=none,id=d,driver=null-co,"
                                 "size=1M -device usb-storage,"
                                 "bus=usb-c.0,port=1,drive=d");
    uint32_t v = port_reset(qts, USB1_BASE);

    g_assert_cmphex(v & PORTSC_PED, ==, PORTSC_PED);
    g_assert_cmpuint(PORTSC_PSPD(v), ==, 2);
    qtest_quit(qts);
}

/*
 * Without a port number QEMU puts its (full speed) hub on the single root
 * port; devices on both buses at the same port path must not collide.
 */
static void test_both_buses(void)
{
    QTestState *qts = qtest_init("-M kick -device usb-kbd,bus=usb-a.0 "
                                 "-device usb-kbd,bus=usb-c.0");
    QDict *rsp;
    const char *info;

    rsp = qtest_qmp(qts, "{'execute': 'human-monitor-command', 'arguments':"
                    " {'command-line': 'info usb'}}");
    info = qdict_get_str(rsp, "return");
    g_assert_nonnull(strstr(info, "Port 1.1, Speed 12 Mb/s, Product QEMU USB "
                                  "Keyboard"));
    g_assert_cmpuint(PORTSC_PSPD(port_reset(qts, USB2_BASE)), ==, 0);
    qobject_unref(rsp);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/usb/registers", test_registers);
    qtest_add_func("/kick/usb/full-speed-port", test_full_speed_port);
    qtest_add_func("/kick/usb/high-speed-port", test_high_speed_port);
    qtest_add_func("/kick/usb/both-buses", test_both_buses);
    return g_test_run();
}
