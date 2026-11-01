/*
 * qtest for the i.MX8MM SAI model (SAI2 of the kick-periph-test machine)
 *
 * The frame engine is paced by the virtual clock, the audiodev is the
 * "none" backend libqtest configures.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define SAI             0x30020000
#define SAI_PATH        "/machine/sai2"

#define VERID           0x000
#define PARAM           0x004
#define TCSR            0x008
#define TCR1            0x00c
#define TCR2            0x010
#define TCR3            0x014
#define TCR4            0x018
#define TCR5            0x01c
#define TDR0            0x020
#define TFR0            0x040
#define TMR             0x060
#define RCSR            0x088
#define RCR1            0x08c
#define RCR2            0x090
#define RCR3            0x094
#define RCR4            0x098
#define RCR5            0x09c
#define RDR0            0x0a0
#define RFR0            0x0c0
#define RMR             0x0e0
#define MCTL            0x100
#define MDIV            0x104

#define CSR_TE          (1u << 31)
#define CSR_FR          (1u << 25)
#define CSR_SR          (1u << 24)
#define CSR_FEF         (1u << 18)
#define CSR_FWF         (1u << 17)
#define CSR_FRF         (1u << 16)
#define CSR_FEIE        (1u << 10)
#define CSR_FRDE        (1u << 0)

#define MS              (1000 * 1000)

static QTestState *sai_init(void)
{
    return qtest_init("-M kick-periph-test");
}

static void wr(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, SAI + off, val);
}

static uint32_t rd(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, SAI + off);
}

/*
 * What fsl_sai programs for 2 channels S32_LE I2S at 48 kHz as clock
 * master from MCLK1 = 12.288 MHz: BCLK = 12.288 MHz / ((1 + 1) * 2).
 */
static void sai_setup_i2s(QTestState *qts, uint32_t base)
{
    wr(qts, base + 0x08, (1u << 26) | (1u << 24) | 1);  /* MSEL1, BCD, DIV */
    wr(qts, base + 0x0c, 1u << 16);                     /* TCE/RCE line 0 */
    wr(qts, base + 0x10, (1u << 16) | (31u << 8) | (1 << 5) | (1 << 4) |
       (1 << 3) | (1 << 1) | 1);    /* FRSZ 2, SYWD 32, CHMOD, MF, FSE, FSP, FSD */
    wr(qts, base + 0x14, (31u << 24) | (31u << 16) | (31u << 8));
    wr(qts, base + 0x58, 0xfffffffc);                   /* xMR */
}

static void test_reset_values(void)
{
    QTestState *qts = sai_init();

    g_assert_cmphex(rd(qts, VERID), ==, 0x03010000);
    g_assert_cmphex(rd(qts, PARAM), ==, 0x00050708);    /* 32 slots, 128 w, 8 */
    g_assert_cmphex(rd(qts, TCSR), ==, 0);
    g_assert_cmphex(rd(qts, RCSR), ==, 0);
    g_assert_cmphex(rd(qts, TCR2), ==, 0);
    g_assert_cmphex(rd(qts, TFR0), ==, 0);
    g_assert_cmphex(rd(qts, RFR0), ==, 0);
    g_assert_cmphex(rd(qts, TMR), ==, 0);
    g_assert_cmphex(rd(qts, MCTL), ==, 0);
    g_assert_cmphex(rd(qts, MDIV), ==, 0);
    /* Write masks */
    wr(qts, TCR1, 0xffffffff);
    g_assert_cmphex(rd(qts, TCR1), ==, 0x7f);
    wr(qts, MCTL, 0xffffffff);
    g_assert_cmphex(rd(qts, MCTL), ==, 0x438000ff);
    qtest_quit(qts);
}

static void test_fifo_dma_request(void)
{
    QTestState *qts = sai_init();
    int i;

    qtest_irq_intercept_out_named(qts, SAI_PATH, "dma-tx");
    wr(qts, TCR3, 1u << 16);
    wr(qts, TCR1, 122);             /* fifo_depth - maxburst */
    for (i = 0; i < 3; i++) {
        wr(qts, TDR0, i);
    }
    g_assert_cmphex(rd(qts, TFR0), ==, 0x00030000);
    g_assert_cmphex(rd(qts, TCSR) & (CSR_FRF | CSR_FWF), ==, CSR_FRF);
    g_assert_false(qtest_get_irq(qts, 0));

    /* FRDE: request while the FIFO is at or below the watermark */
    wr(qts, TCSR, CSR_FRDE);
    g_assert_true(qtest_get_irq(qts, 0));
    for (i = 0; i < 120; i++) {
        wr(qts, TDR0, i);
    }
    g_assert_cmphex(rd(qts, TFR0), ==, 0x007b0000);    /* 123 words */
    g_assert_cmphex(rd(qts, TCSR) & CSR_FRF, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* A full FIFO ignores writes; FR resets the pointers */
    for (i = 0; i < 10; i++) {
        wr(qts, TDR0, i);
    }
    g_assert_cmphex(rd(qts, TFR0), ==, 0x00800000);
    wr(qts, TCSR, CSR_FRDE | CSR_FR);
    g_assert_cmphex(rd(qts, TFR0), ==, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, TCSR) & CSR_FR, ==, 0);    /* self clearing */
    qtest_quit(qts);
}

static void test_playback_rate_underrun(void)
{
    QTestState *qts = sai_init();
    int i;

    qtest_irq_intercept_out_named(qts, SAI_PATH, "sysbus-irq");
    sai_setup_i2s(qts, 0x08);
    wr(qts, TCR1, 122);
    for (i = 0; i < 128; i++) {
        wr(qts, TDR0, 0x10000 * i);
    }
    wr(qts, TCSR, CSR_TE | CSR_FEIE);

    /* 1 ms at 48 kHz: 48 frames of 2 words */
    qtest_clock_step(qts, 1 * MS);
    g_assert_cmphex(rd(qts, TFR0), ==, 0x00800060);
    g_assert_cmphex(rd(qts, TCSR) & CSR_FEF, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Nobody refills: the FIFO runs dry and FEF raises the interrupt */
    qtest_clock_step(qts, 1 * MS);
    g_assert_cmphex(rd(qts, TCSR) & CSR_FEF, ==, CSR_FEF);
    g_assert_true(qtest_get_irq(qts, 0));
    wr(qts, TCSR, CSR_TE | CSR_FEIE | CSR_FEF);         /* W1C */
    g_assert_false(qtest_get_irq(qts, 0));

    /* Disabling TE stops the frame engine */
    wr(qts, TCSR, 0);
    g_assert_cmphex(rd(qts, TCSR) & CSR_TE, ==, 0);
    wr(qts, TDR0, 1);
    qtest_clock_step(qts, 1 * MS);
    g_assert_cmphex(rd(qts, TFR0) >> 16, ==, (rd(qts, TFR0) & 0xff) + 1);
    qtest_quit(qts);
}

static void test_capture_synchronous(void)
{
    QTestState *qts = sai_init();
    uint32_t rfr;

    /* fsl,sai-synchronous-rx: TX is synchronous to RX, RX makes the clocks */
    qtest_irq_intercept_out_named(qts, SAI_PATH, "dma-rx");
    sai_setup_i2s(qts, 0x88);
    wr(qts, RCR1, 5);                       /* maxburst - 1 */
    wr(qts, RCSR, CSR_TE | CSR_FRDE);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 1 * MS);
    rfr = rd(qts, RFR0);
    g_assert_cmphex(rfr >> 16, ==, 96);     /* 48 frames received */
    g_assert_cmphex(rd(qts, RCSR) & CSR_FRF, ==, CSR_FRF);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, RDR0), ==, 0);  /* silence from the backend */
    g_assert_cmphex(rd(qts, RFR0) & 0xff, ==, 1);

    /* Overflow: FIFO full, FEF set */
    qtest_clock_step(qts, 2 * MS);
    g_assert_cmphex(rd(qts, RCSR) & (CSR_FEF | CSR_FWF), ==,
                    CSR_FEF | CSR_FWF);

    /* Software reset clears FIFO and flags */
    wr(qts, RCSR, CSR_SR);
    wr(qts, RCSR, 0);
    g_assert_cmphex(rd(qts, RFR0), ==, 0);
    g_assert_cmphex(rd(qts, RCSR), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_sync_needs_clock_side(void)
{
    QTestState *qts = sai_init();

    /* TX synchronous to RX does not run while RX is disabled */
    sai_setup_i2s(qts, 0x88);
    sai_setup_i2s(qts, 0x08);
    wr(qts, TCR2, 1u << 30);
    wr(qts, TDR0, 1);
    wr(qts, TDR0, 2);
    wr(qts, TCSR, CSR_TE);
    qtest_clock_step(qts, 1 * MS);
    g_assert_cmphex(rd(qts, TFR0), ==, 0x00020000);

    wr(qts, RCSR, CSR_TE);
    qtest_clock_step(qts, 1 * MS);
    g_assert_cmphex(rd(qts, TFR0) & 0xff, ==, 2);
    qtest_quit(qts);
}

/*
 * Data line 0 reaches the audiodev: record it with the wav backend (16 bit,
 * so the S32 samples arrive as their upper half).
 */
static void test_playback_to_audiodev(void)
{
    g_autofree char *path = NULL;
    g_autofree char *wav = NULL;
    QTestState *qts;
    gsize len;
    int fd = g_file_open_tmp("kick-sai-XXXXXX.wav", &path, NULL);
    int i, first = -1;
    const int16_t *pcm;
    size_t n;

    g_assert(fd >= 0);
    close(fd);
    qts = qtest_initf("-M kick-periph-test,audiodev=w -audiodev wav,id=w,"
                      "path=%s,out.frequency=48000,out.channels=2,"
                      "out.format=s16", path);
    sai_setup_i2s(qts, 0x08);
    /* Left = (n + 1) << 20, right = -left, exact through the mixer */
    for (i = 0; i < 64; i++) {
        wr(qts, TDR0, (uint32_t)((i + 1) << 20));
        wr(qts, TDR0, (uint32_t)(-((i + 1) << 20)));
    }
    wr(qts, TCSR, CSR_TE);
    qtest_clock_step(qts, 100 * MS);
    qtest_quit(qts);

    g_assert(g_file_get_contents(path, &wav, &len, NULL));
    unlink(path);
    g_assert_cmpuint(len, >, 44);
    pcm = (const int16_t *)(wav + 44);
    n = (len - 44) / 2;
    for (i = 0; i < (int)n; i += 2) {
        if (pcm[i]) {
            first = i;
            break;
        }
    }
    g_assert_cmpint(first, >=, 0);
    g_assert_cmpuint(n - first, >=, 128);
    /* The float mixer may round the 16 bit result by one LSB */
    for (i = 0; i < 64; i++) {
        g_assert_cmpint(abs(GINT16_FROM_LE(pcm[first + 2 * i]) -
                            ((i + 1) << 4)), <=, 1);
        g_assert_cmpint(abs(GINT16_FROM_LE(pcm[first + 2 * i + 1]) +
                            ((i + 1) << 4)), <=, 1);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/sai/reset-values", test_reset_values);
    qtest_add_func("/kick/sai/fifo-dma-request", test_fifo_dma_request);
    qtest_add_func("/kick/sai/playback-rate-underrun",
                   test_playback_rate_underrun);
    qtest_add_func("/kick/sai/capture-synchronous", test_capture_synchronous);
    qtest_add_func("/kick/sai/sync-clock-side", test_sync_needs_clock_side);
    qtest_add_func("/kick/sai/playback-audiodev", test_playback_to_audiodev);
    return g_test_run();
}
