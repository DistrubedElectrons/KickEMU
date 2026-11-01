/*
 * qtest for the i.MX8MM SDMA model (SDMA2 of the kick-periph-test machine)
 *
 * Follows the register and memory sequences of the Linux 5.4 imx-sdma
 * driver without RAM firmware: channel 0 boot commands, context load,
 * ap_2_ap memcpy and mcu_2_shp feeding SAI2 through DMA event 3.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define SDMA            0x302c0000
#define SDMA_PATH       "/machine/sdma2"
#define SAI             0x30020000

#define MC0PTR          0x000
#define INTR            0x004
#define STOP_STAT       0x008
#define HSTART          0x00c
#define EVTOVR          0x010
#define DSPOVR          0x014
#define HOSTOVR         0x018
#define EVTPEND         0x01c
#define RESET           0x024
#define EVTERR          0x028
#define CONFIG          0x038
#define ILLINSTADDR     0x058
#define CHN0ADDR        0x05c
#define CHNPRI(n)       (0x100 + 4 * (n))
#define CHNENBL(n)      (0x200 + 4 * (n))

#define BD_DONE         0x01
#define BD_WRAP         0x02
#define BD_CONT         0x04
#define BD_INTR         0x08
#define BD_RROR         0x10
#define BD_LAST         0x20
#define BD_EXTD         0x80

#define C0_SETDM        0x01
#define C0_GETDM        0x02
#define C0_SETPM        0x04
#define C0_GETPM        0x08

/* ROM script entry points of the i.MX 7D/8M SDMA */
#define AP_2_AP         644
#define MCU_2_SHP       962

/* Guest memory layout */
#define RAM             0x40000000
#define CCB             RAM                     /* 32 x 16 bytes */
#define CTX             (CCB + 32 * 16)         /* context buffer */
#define BD0             (RAM + 0x1000)
#define BD_CH           (RAM + 0x1100)
#define BUF_A           (RAM + 0x2000)
#define BUF_B           (RAM + 0x3000)

#define CTX_WORDS       32

static uint32_t sdma_rd(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, SDMA + off);
}

static void sdma_wr(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, SDMA + off, val);
}

static void write_bd(QTestState *qts, uint32_t addr, uint16_t count,
                     uint8_t status, uint8_t cmd, uint32_t buf, uint32_t ext)
{
    qtest_writel(qts, addr, count | (status << 16) | ((uint32_t)cmd << 24));
    qtest_writel(qts, addr + 4, buf);
    qtest_writel(qts, addr + 8, ext);
}

static uint8_t bd_status(QTestState *qts, uint32_t addr)
{
    return qtest_readl(qts, addr) >> 16;
}

/* sdma_runtime_resume() + sdma_request_channel0() */
static QTestState *sdma_init(void)
{
    QTestState *qts = qtest_init("-M kick-periph-test");
    int i;

    sdma_wr(qts, MC0PTR, 0);
    for (i = 0; i < 48; i++) {
        sdma_wr(qts, CHNENBL(i), 0);
    }
    for (i = 0; i < 32; i++) {
        sdma_wr(qts, CHNPRI(i), 0);
    }
    qtest_writel(qts, CCB + 0, BD0);            /* current */
    qtest_writel(qts, CCB + 4, BD0);            /* base */
    sdma_wr(qts, CHNPRI(0), 1);
    /* sdma_config_ownership(ch0, false, true, false) */
    sdma_wr(qts, EVTOVR, sdma_rd(qts, EVTOVR) | 1);
    sdma_wr(qts, HOSTOVR, sdma_rd(qts, HOSTOVR) & ~1);
    sdma_wr(qts, DSPOVR, sdma_rd(qts, DSPOVR) | 1);
    sdma_wr(qts, CHN0ADDR, 0x4050);
    sdma_wr(qts, CONFIG, 0);
    sdma_wr(qts, MC0PTR, CCB);
    sdma_wr(qts, CHNPRI(0), 7);
    return qts;
}

/* sdma_run_channel0(): returns with the command done */
static void run_channel0(QTestState *qts, uint8_t cmd, uint16_t count,
                         uint32_t buf, uint32_t ext)
{
    write_bd(qts, BD0, count, BD_DONE | BD_WRAP | BD_EXTD, cmd, buf, ext);
    sdma_wr(qts, HSTART, 1);
    g_assert_cmphex(sdma_rd(qts, STOP_STAT) & 1, ==, 0);
    g_assert_cmphex(bd_status(qts, BD0) & BD_DONE, ==, 0);
}

/* sdma_load_context(): PC and gReg via C0_SETDM to 2048 + 32 * ch */
static void load_context(QTestState *qts, int ch, uint32_t pc,
                         const uint32_t greg[8])
{
    int i;

    for (i = 0; i < CTX_WORDS; i++) {
        qtest_writel(qts, CTX + i * 4, 0);
    }
    qtest_writel(qts, CTX, pc);
    for (i = 0; i < 8; i++) {
        qtest_writel(qts, CTX + 4 + i * 4, greg[i]);
    }
    run_channel0(qts, C0_SETDM, CTX_WORDS, CTX, 2048 + CTX_WORDS * ch);
}

static bool wait_intr(QTestState *qts, uint32_t mask)
{
    int i;

    for (i = 0; i < 100; i++) {
        if (sdma_rd(qts, INTR) & mask) {
            return true;
        }
        qtest_clock_step(qts, 10 * 1000);
    }
    return false;
}

static void test_reset_values(void)
{
    QTestState *qts = qtest_init("-M kick-periph-test");

    g_assert_cmphex(sdma_rd(qts, MC0PTR), ==, 0);
    g_assert_cmphex(sdma_rd(qts, INTR), ==, 0);
    g_assert_cmphex(sdma_rd(qts, STOP_STAT), ==, 0);
    g_assert_cmphex(sdma_rd(qts, EVTOVR), ==, 0);
    g_assert_cmphex(sdma_rd(qts, DSPOVR), ==, 0xffffffff);
    g_assert_cmphex(sdma_rd(qts, HOSTOVR), ==, 0);
    g_assert_cmphex(sdma_rd(qts, EVTPEND), ==, 0);
    g_assert_cmphex(sdma_rd(qts, CONFIG), ==, 3);
    g_assert_cmphex(sdma_rd(qts, ILLINSTADDR), ==, 1);
    g_assert_cmphex(sdma_rd(qts, CHN0ADDR), ==, 0x50);
    g_assert_cmphex(sdma_rd(qts, CHNPRI(5)), ==, 0);
    g_assert_cmphex(sdma_rd(qts, CHNENBL(47)), ==, 0);
    qtest_quit(qts);
}

static void test_channel0_load(void)
{
    QTestState *qts = sdma_init();
    uint32_t greg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int i;

    /* RAM script download (C0_SETPM) and read back (C0_GETPM) */
    for (i = 0; i < 16; i++) {
        qtest_writew(qts, BUF_A + i * 2, 0x1000 + i);
    }
    run_channel0(qts, C0_SETPM, 16, BUF_A, 0x1800);
    run_channel0(qts, C0_GETPM, 16, BUF_B, 0x1800);
    for (i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readw(qts, BUF_B + i * 2), ==, 0x1000 + i);
    }
    /* Program and data memory are one RAM: PM 0x1800 = DM 0xc00 */
    run_channel0(qts, C0_GETDM, 1, BUF_B, 0xc00);
    g_assert_cmphex(qtest_readl(qts, BUF_B), ==, 0x10001001);

    /* Context of channel 3 through C0_SETDM, read back with C0_GETDM */
    load_context(qts, 3, AP_2_AP, greg);
    run_channel0(qts, C0_GETDM, CTX_WORDS, BUF_B, 2048 + CTX_WORDS * 3);
    g_assert_cmphex(qtest_readl(qts, BUF_B), ==, AP_2_AP);
    g_assert_cmphex(qtest_readl(qts, BUF_B + 4 * 7), ==, 7);
    g_assert_cmphex(sdma_rd(qts, INTR), ==, 0);
    qtest_quit(qts);
}

static void test_memcpy(void)
{
    QTestState *qts = sdma_init();
    const uint32_t greg[8] = { 0 };
    int i;

    qtest_irq_intercept_out_named(qts, SDMA_PATH, "sysbus-irq");
    for (i = 0; i < 96; i++) {
        qtest_writeb(qts, BUF_A + i, i ^ 0x5a);
        qtest_writeb(qts, BUF_B + i, 0);
    }
    /* Memory channel: events overridden, host start only */
    sdma_wr(qts, EVTOVR, sdma_rd(qts, EVTOVR) | (1 << 1));
    sdma_wr(qts, HOSTOVR, sdma_rd(qts, HOSTOVR) & ~(1 << 1));
    sdma_wr(qts, DSPOVR, sdma_rd(qts, DSPOVR) | (1 << 1));
    load_context(qts, 1, AP_2_AP, greg);
    sdma_wr(qts, CHNPRI(1), 1);

    /* sdma_prep_memcpy(): chained BDs, the last one interrupts */
    write_bd(qts, BD_CH, 64, BD_DONE | BD_EXTD | BD_CONT, 0, BUF_A, BUF_B);
    write_bd(qts, BD_CH + 12, 32, BD_DONE | BD_EXTD | BD_INTR | BD_LAST, 0,
             BUF_A + 64, BUF_B + 64);
    qtest_writel(qts, CCB + 16, BD_CH);
    qtest_writel(qts, CCB + 20, BD_CH);
    sdma_wr(qts, HSTART, 1 << 1);

    g_assert_true(wait_intr(qts, 1 << 1));
    g_assert_true(qtest_get_irq(qts, 0));
    for (i = 0; i < 96; i++) {
        g_assert_cmphex(qtest_readb(qts, BUF_B + i), ==, i ^ 0x5a);
    }
    g_assert_cmphex(bd_status(qts, BD_CH) & BD_DONE, ==, 0);
    g_assert_cmphex(bd_status(qts, BD_CH + 12) & (BD_DONE | BD_RROR), ==, 0);
    g_assert_cmphex(sdma_rd(qts, STOP_STAT) & (1 << 1), ==, 0);

    sdma_wr(qts, INTR, 1 << 1);                 /* W1C */
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void sai_setup_tx(QTestState *qts)
{
    qtest_writel(qts, SAI + 0x010, (1u << 26) | (1u << 24) | 1);
    qtest_writel(qts, SAI + 0x014, 1u << 16);
    qtest_writel(qts, SAI + 0x018, (1u << 16) | (31u << 8) | 0x3b);
    qtest_writel(qts, SAI + 0x01c, (31u << 24) | (31u << 16) | (31u << 8));
    qtest_writel(qts, SAI + 0x060, 0xfffffffc);
    qtest_writel(qts, SAI + 0x00c, 122);
}

static void test_mcu_2_shp_sai(void)
{
    QTestState *qts = sdma_init();
    /* gReg[1] = event mask (event 3), gReg[6] = TDR0, gReg[7] = 6 x 4 B */
    const uint32_t greg[8] = { 0, 1 << 3, 0, 0, 0, 0, SAI + 0x20, 24 };
    const int periods = 4, period = 256;
    uint32_t tfr;
    int i;

    for (i = 0; i < periods * period / 4; i++) {
        qtest_writel(qts, BUF_A + i * 4, 0x01000000 + i);
    }
    sai_setup_tx(qts);

    /* sdma_config(): event 3 enabled for channel 2 before anything else */
    sdma_wr(qts, CHNENBL(3), sdma_rd(qts, CHNENBL(3)) | (1 << 2));
    sdma_wr(qts, EVTOVR, sdma_rd(qts, EVTOVR) & ~(1 << 2));
    sdma_wr(qts, HOSTOVR, sdma_rd(qts, HOSTOVR) & ~(1 << 2));
    sdma_wr(qts, DSPOVR, sdma_rd(qts, DSPOVR) | (1 << 2));
    load_context(qts, 2, MCU_2_SHP, greg);
    sdma_wr(qts, CHNPRI(2), 1);

    /* sdma_prep_dma_cyclic() */
    for (i = 0; i < periods; i++) {
        write_bd(qts, BD_CH + i * 12, period,
                 BD_DONE | BD_EXTD | BD_CONT | BD_INTR |
                 (i == periods - 1 ? BD_WRAP : 0), 0,
                 BUF_A + i * period, 0);
    }
    qtest_writel(qts, CCB + 32, BD_CH);
    qtest_writel(qts, CCB + 36, BD_CH);
    sdma_wr(qts, HSTART, 1 << 2);

    /* Nothing happens until the SAI requests data */
    qtest_clock_step(qts, 100 * 1000);
    g_assert_cmphex(qtest_readl(qts, SAI + 0x040), ==, 0);

    /* fsl_sai_trigger(): FRDE, the DMA fills the FIFO above the watermark */
    qtest_writel(qts, SAI + 0x008, 1);
    g_assert_true(wait_intr(qts, 1 << 2));
    tfr = qtest_readl(qts, SAI + 0x040);
    /* Bursts of 6 words, the one ending BD0 is cut at 64: 60 + 4 + 60 */
    g_assert_cmpuint(tfr >> 16, ==, 124);
    g_assert_cmphex(bd_status(qts, BD_CH) & BD_DONE, ==, 0);
    g_assert_cmphex(bd_status(qts, BD_CH + 12) & BD_DONE, ==, BD_DONE);

    /* Start the transmitter: 1 ms consumes 96 words, the DMA keeps up */
    sdma_wr(qts, INTR, 1 << 2);
    qtest_writel(qts, SAI + 0x008, (1u << 31) | 1);
    qtest_clock_step(qts, 1000 * 1000);
    tfr = qtest_readl(qts, SAI + 0x040);
    g_assert_cmpuint(tfr & 0xff, ==, 96);
    g_assert_cmpuint(((tfr >> 16) - (tfr & 0xff)) & 0xff, >, 122);
    g_assert_cmphex(qtest_readl(qts, SAI + 0x008) & (1 << 18), ==, 0);
    g_assert_cmphex(sdma_rd(qts, INTR) & (1 << 2), ==, 1 << 2);
    /* About 220 words written: BD0..BD2 done, BD3 in progress */
    g_assert_cmphex(bd_status(qts, BD_CH + 24) & BD_DONE, ==, 0);
    g_assert_cmphex(bd_status(qts, BD_CH + 36) & BD_DONE, ==, BD_DONE);

    /*
     * The CPU never hands the BDs back: once BD3 is done the channel waits
     * at BD0 and the SAI underruns.
     */
    qtest_clock_step(qts, 5 * 1000 * 1000);
    g_assert_cmphex(qtest_readl(qts, SAI + 0x008) & (1 << 18), ==, 1 << 18);

    /* Re-arming BD0 resumes the transfer */
    write_bd(qts, BD_CH, period, BD_DONE | BD_EXTD | BD_CONT | BD_INTR, 0,
             BUF_A, 0);
    tfr = qtest_readl(qts, SAI + 0x040);
    qtest_clock_step(qts, 200 * 1000);
    g_assert_cmpuint(qtest_readl(qts, SAI + 0x040) >> 16, !=, tfr >> 16);
    qtest_quit(qts);
}

static void test_unknown_script(void)
{
    QTestState *qts = sdma_init();
    const uint32_t greg[8] = { 0 };

    /* A RAM script address (no firmware loaded): transfer fails visibly */
    sdma_wr(qts, EVTOVR, sdma_rd(qts, EVTOVR) | (1 << 4));
    load_context(qts, 4, 0x1822, greg);
    sdma_wr(qts, CHNPRI(4), 1);
    write_bd(qts, BD_CH, 16, BD_DONE | BD_EXTD | BD_INTR, 0, BUF_A, BUF_B);
    qtest_writel(qts, CCB + 64, BD_CH);
    qtest_writel(qts, CCB + 68, BD_CH);
    sdma_wr(qts, HSTART, 1 << 4);
    g_assert_true(wait_intr(qts, 1 << 4));
    g_assert_cmphex(bd_status(qts, BD_CH) & (BD_DONE | BD_RROR), ==, BD_RROR);
    g_assert_cmphex(sdma_rd(qts, STOP_STAT) & (1 << 4), ==, 0);
    qtest_quit(qts);
}

static void test_event_error(void)
{
    QTestState *qts = sdma_init();

    /* Event 3 for channel 5 without a started channel: second edge = error */
    sdma_wr(qts, CHNENBL(3), 1 << 5);
    qtest_set_irq_in(qts, SDMA_PATH, "events", 3, 1);
    g_assert_cmphex(sdma_rd(qts, EVTPEND), ==, 1 << 5);
    qtest_set_irq_in(qts, SDMA_PATH, "events", 3, 0);
    qtest_set_irq_in(qts, SDMA_PATH, "events", 3, 1);
    g_assert_cmphex(sdma_rd(qts, EVTERR), ==, 1 << 5);
    g_assert_cmphex(sdma_rd(qts, EVTERR), ==, 0);      /* clear on read */
    sdma_wr(qts, EVTPEND, 1 << 5);
    g_assert_cmphex(sdma_rd(qts, EVTPEND), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/sdma/reset-values", test_reset_values);
    qtest_add_func("/kick/sdma/channel0-load", test_channel0_load);
    qtest_add_func("/kick/sdma/memcpy", test_memcpy);
    qtest_add_func("/kick/sdma/mcu-2-shp-sai", test_mcu_2_shp_sai);
    qtest_add_func("/kick/sdma/unknown-script", test_unknown_script);
    qtest_add_func("/kick/sdma/event-error", test_event_error);
    return g_test_run();
}
