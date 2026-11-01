/*
 * qtest for the i.MX8MM eLCDIF model
 *
 * Runs on the kick-display-bench machine, which maps the LCDIF at its
 * i.MX8MM address. Checks reset values, the SET/CLR/TOG aliases, soft reset,
 * the frame interrupts and the scanout of a known pattern via screendump.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "libqtest.h"

#define LCDIF_BASE          0x32e00000
#define RAM_BASE            0x40000000
#define FB0                 (RAM_BASE + 0x100000)
#define FB1                 (RAM_BASE + 0x200000)

#define CTRL                0x000
#define CTRL1               0x010
#define CTRL2               0x020
#define TRANSFER_COUNT      0x030
#define CUR_BUF             0x040
#define NEXT_BUF            0x050
#define VDCTRL0             0x070
#define VDCTRL1             0x080
#define VDCTRL2             0x090
#define VDCTRL3             0x0a0
#define VDCTRL4             0x0b0
#define STAT                0x1b0
#define PIGEON_12_0         0xb00
#define PIGEON_12_1         0xb10
#define SET                 0x4
#define CLR                 0x8
#define TOG                 0xc

#define CTRL_SFTRST         (1u << 31)
#define CTRL_CLKGATE        (1u << 30)
#define CTRL_DOTCLK_MODE    (1u << 17)
#define CTRL_BYPASS_COUNT   (1u << 19)
#define CTRL_MASTER         (1u << 5)
#define CTRL_DF16           (1u << 3)
#define CTRL_RUN            (1u << 0)
#define CTRL1_VSYNC_IRQ     (1u << 8)
#define CTRL1_DONE_IRQ      (1u << 9)
#define CTRL1_DONE_IRQ_EN   (1u << 13)

#define W                   64
#define H                   32

static const char *lcdif_path = "/machine/lcdif";

static uint32_t rd(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, LCDIF_BASE + off);
}

static void wr(QTestState *qts, uint32_t off, uint32_t v)
{
    qtest_writel(qts, LCDIF_BASE + off, v);
}

static QTestState *start(void)
{
    return qtest_init("-machine kick-display-bench -display none");
}

static void test_reset_values(void)
{
    QTestState *qts = start();

    g_assert_cmphex(rd(qts, CTRL), ==, CTRL_SFTRST | CTRL_CLKGATE);
    g_assert_cmphex(rd(qts, CTRL1), ==, 0x000f0000);
    g_assert_cmphex(rd(qts, CTRL2), ==, 0x00200000);
    g_assert_cmphex(rd(qts, TRANSFER_COUNT), ==, 0x00010000);
    g_assert_cmphex(rd(qts, CUR_BUF), ==, 0);
    g_assert_cmphex(rd(qts, NEXT_BUF), ==, 0);
    g_assert_cmphex(rd(qts, VDCTRL0), ==, 0);
    g_assert_cmphex(rd(qts, STAT) & 0x80000000, ==, 0x80000000);
    qtest_quit(qts);
}

static void test_set_clr_tog(void)
{
    QTestState *qts = start();

    /* Out of reset, as the driver does on runtime resume. */
    wr(qts, CTRL, 0);
    g_assert_cmphex(rd(qts, CTRL), ==, 0);

    wr(qts, CTRL1 + SET, CTRL1_DONE_IRQ_EN);
    g_assert_cmphex(rd(qts, CTRL1), ==, 0x000f0000 | CTRL1_DONE_IRQ_EN);
    wr(qts, CTRL1 + CLR, 0x000f0000);
    g_assert_cmphex(rd(qts, CTRL1), ==, CTRL1_DONE_IRQ_EN);
    wr(qts, CTRL1 + TOG, CTRL1_DONE_IRQ_EN | 0x00070000);
    g_assert_cmphex(rd(qts, CTRL1), ==, 0x00070000);

    wr(qts, VDCTRL0 + SET, 0x10000003);
    wr(qts, VDCTRL0 + CLR, 0x00000001);
    g_assert_cmphex(rd(qts, VDCTRL0), ==, 0x10000002);

    /* Plain registers have no aliases. */
    wr(qts, TRANSFER_COUNT, 0x01e00320);
    g_assert_cmphex(rd(qts, TRANSFER_COUNT), ==, 0x01e00320);

    /* Soft reset restores the defaults and gates the clock. */
    wr(qts, CTRL + SET, CTRL_SFTRST);
    g_assert_cmphex(rd(qts, CTRL), ==, CTRL_SFTRST | CTRL_CLKGATE);
    g_assert_cmphex(rd(qts, CTRL1), ==, 0x000f0000);
    g_assert_cmphex(rd(qts, TRANSFER_COUNT), ==, 0x00010000);
    /* Writes are ignored while the block is in reset. */
    wr(qts, TRANSFER_COUNT, 0x12345678);
    g_assert_cmphex(rd(qts, TRANSFER_COUNT), ==, 0x00010000);
    qtest_quit(qts);
}

/*
 * Program the controller the way lcdif_set_mode()/lcdif_set_pix_fmt()/
 * lcdif_enable_controller() do for a W x H mode with small porches.
 */
static void program_mode(QTestState *qts, uint32_t ctrl_fmt, uint32_t packing,
                         uint32_t pattern, uint32_t fb)
{
    uint32_t htotal = W + 8 + 8 + 4, vtotal = H + 2 + 2 + 1;

    wr(qts, CTRL, 0);
    wr(qts, TRANSFER_COUNT, (H << 16) | W);
    wr(qts, VDCTRL0, (1u << 28) | (1u << 21) | (1u << 20) | 1);
    wr(qts, VDCTRL1, vtotal);
    wr(qts, VDCTRL2, (4u << 18) | htotal);
    wr(qts, VDCTRL3, ((4 + 8) << 16) | (1 + 2));
    wr(qts, VDCTRL4, W | (1u << 18));
    wr(qts, CTRL1 + CLR, 0xf << 16);
    wr(qts, CTRL1 + SET, packing << 16);
    wr(qts, CTRL2 + CLR, (7 << 16) | (7 << 12));
    wr(qts, CTRL2 + SET, (pattern << 16) | (pattern << 12));
    wr(qts, CTRL + SET, ctrl_fmt | (3 << 10));
    wr(qts, NEXT_BUF, fb);
    wr(qts, CTRL + SET, CTRL_BYPASS_COUNT | CTRL_DOTCLK_MODE);
    wr(qts, CTRL + SET, CTRL_MASTER);
    wr(qts, CTRL + SET, CTRL_RUN);
}

static uint8_t *screendump(QTestState *qts, int *w, int *h)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    gsize len;
    int fd, maxval, off;
    uint8_t *pix;

    fd = g_file_open_tmp("kick-lcdif-XXXXXX.ppm", &path, NULL);
    g_assert(fd >= 0);
    close(fd);
    qtest_qmp_assert_success(qts, "{'execute': 'screendump', "
                             "'arguments': {'filename': %s}}", path);
    g_assert(g_file_get_contents(path, &contents, &len, NULL));
    unlink(path);
    g_assert(sscanf(contents, "P6 %d %d %d%n", w, h, &maxval, &off) == 3);
    g_assert_cmpint(maxval, ==, 255);
    off++;
    g_assert_cmpint(len, >=, off + *w * *h * 3);
    pix = g_malloc(*w * *h * 3);
    memcpy(pix, contents + off, *w * *h * 3);
    return pix;
}

static uint32_t pattern_rgb(int x, int y)
{
    return ((x * 4) << 16) | ((y * 8) << 8) | ((x + y) & 0xff);
}

static void check_dump(QTestState *qts, int width)
{
    int w, h, x, y;
    g_autofree uint8_t *pix = screendump(qts, &w, &h);

    g_assert_cmpint(w, ==, width);
    g_assert_cmpint(h, ==, H);
    for (y = 0; y < H; y++) {
        for (x = 0; x < width; x++) {
            uint32_t want = pattern_rgb(x, y);
            uint8_t *p = pix + (y * w + x) * 3;
            uint32_t got = (p[0] << 16) | (p[1] << 8) | p[2];
            g_assert_cmphex(got, ==, want);
        }
    }
}

static void test_scanout_xrgb8888(void)
{
    QTestState *qts = start();
    int x, y;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            qtest_writel(qts, FB0 + (y * W + x) * 4,
                         0xff000000 | pattern_rgb(x, y));
        }
    }
    program_mode(qts, 3 << 8, 0x7, 0, FB0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(rd(qts, CUR_BUF), ==, FB0);
    check_dump(qts, W);
    qtest_quit(qts);
}

static void test_scanout_abgr8888(void)
{
    QTestState *qts = start();
    int x, y;

    /* ABGR8888 in memory, CTRL2 line pattern BGR. */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t c = pattern_rgb(x, y);
            uint32_t abgr = 0xff000000 | ((c & 0xff) << 16) | (c & 0xff00) |
                            ((c >> 16) & 0xff);
            qtest_writel(qts, FB0 + (y * W + x) * 4, abgr);
        }
    }
    program_mode(qts, 3 << 8, 0x7, 5, FB0);
    qtest_clock_step(qts, 1);
    check_dump(qts, W);
    qtest_quit(qts);
}

static void test_scanout_rgb565_crop(void)
{
    QTestState *qts = start();
    int x, y;

    /* Pattern values chosen so that RGB565 represents them exactly. */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t c = pattern_rgb(x, y);
            uint16_t p = (((c >> 16) & 0xf8) << 8) | (((c >> 8) & 0xfc) << 3) |
                         ((c & 0xf8) >> 3);
            qtest_writew(qts, FB0 + (y * W + x) * 2, p);
        }
    }
    program_mode(qts, 0, 0xf, 0, FB0);
    /* lcdif_set_fb_hcrop(): only the first 40 pixels are shown. */
    wr(qts, PIGEON_12_0, (0x24u << 24) | 1);
    wr(qts, PIGEON_12_1, 40u << 16);
    qtest_clock_step(qts, 1);
    {
        int w, h;
        g_autofree uint8_t *pix = screendump(qts, &w, &h);

        g_assert_cmpint(w, ==, 40);
        g_assert_cmpint(h, ==, H);
        for (y = 0; y < H; y++) {
            for (x = 0; x < 40; x++) {
                uint32_t c = pattern_rgb(x, y);
                uint8_t *p = pix + (y * w + x) * 3;
                g_assert_cmpint(p[0] & 0xf8, ==, (c >> 16) & 0xf8);
                g_assert_cmpint(p[1] & 0xfc, ==, (c >> 8) & 0xfc);
                g_assert_cmpint(p[2] & 0xf8, ==, c & 0xf8);
            }
        }
    }
    qtest_quit(qts);
}

static void test_frame_irq_and_flip(void)
{
    QTestState *qts = start();
    int64_t period = NANOSECONDS_PER_SECOND / 60;

    qtest_irq_intercept_out_named(qts, lcdif_path, "sysbus-irq");
    program_mode(qts, 3 << 8, 0x7, 0, FB0);

    /* lcdif_vblank_irq_enable() */
    wr(qts, CTRL1 + CLR, CTRL1_DONE_IRQ);
    wr(qts, CTRL1 + SET, CTRL1_DONE_IRQ_EN);

    /* First frame starts immediately: VSYNC edge seen, frame not done yet. */
    qtest_clock_step(qts, 1);
    g_assert_cmphex(rd(qts, CTRL1) & CTRL1_VSYNC_IRQ, ==, CTRL1_VSYNC_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    /* CUR_FRAME_DONE arrives within one frame period and raises the IRQ. */
    qtest_clock_step(qts, period);
    g_assert_cmphex(rd(qts, CTRL1) & CTRL1_DONE_IRQ, ==, CTRL1_DONE_IRQ);
    g_assert_true(qtest_get_irq(qts, 0));

    /* lcdif_vblank_irq_clear() */
    wr(qts, CTRL1 + CLR, CTRL1_DONE_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));

    /* A page flip through NEXT_BUF is latched at the next frame start. */
    wr(qts, NEXT_BUF, FB1);
    g_assert_cmphex(rd(qts, CUR_BUF), ==, FB0);
    qtest_clock_step(qts, period);
    g_assert_cmphex(rd(qts, CUR_BUF), ==, FB1);
    g_assert_true(qtest_get_irq(qts, 0));

    /* lcdif_disable_controller(): RUN reads back clear, frames stop. */
    wr(qts, CTRL + CLR, CTRL_RUN);
    wr(qts, CTRL + CLR, CTRL_DOTCLK_MODE);
    g_assert_cmphex(rd(qts, CTRL) & CTRL_RUN, ==, 0);
    wr(qts, CTRL1 + CLR, CTRL1_DONE_IRQ | CTRL1_VSYNC_IRQ);
    qtest_clock_step(qts, 4 * period);
    g_assert_cmphex(rd(qts, CTRL1) & CTRL1_VSYNC_IRQ, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/lcdif/reset_values", test_reset_values);
    qtest_add_func("/kick/lcdif/set_clr_tog", test_set_clr_tog);
    qtest_add_func("/kick/lcdif/scanout_xrgb8888", test_scanout_xrgb8888);
    qtest_add_func("/kick/lcdif/scanout_abgr8888", test_scanout_abgr8888);
    qtest_add_func("/kick/lcdif/scanout_rgb565_crop", test_scanout_rgb565_crop);
    qtest_add_func("/kick/lcdif/frame_irq_and_flip", test_frame_irq_and_flip);

    return g_test_run();
}
