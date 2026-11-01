/*
 * On-screen intro and live status. The layout is designed on an 800x480
 * grid and scaled to the framebuffer (1024x600 on the KICK panel):
 *
 *   y   0..175  wordmark (wipes in during the first second, then a light
 *               sweep runs across it), subtitle
 *   y 196..440  left: system facts and live counters; right: a mirror of
 *               the 4x4 pads as sent to the MCU
 *   y 456..479  animated rainbow bar
 *
 * Only regions that change are redrawn and written back for the LCDIF.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "fmt.h"
#include "gic.h"
#include "gpt.h"
#include "lcdif.h"
#include "panel.h"
#include "screen.h"
#include "soc_info.h"

#define BG_TOP          RGB(14, 16, 24)
#define BG_BOTTOM       RGB(4, 5, 9)
#define TEXT            RGB(220, 224, 232)
#define TEXT_DIM        RGB(120, 128, 144)
#define ACCENT          RGB(255, 74, 28)

/* Design grid the layout constants below refer to */
#define DESIGN_W        800
#define DESIGN_H        480

/* Layout in framebuffer pixels, computed once by layout_init() */
static int wm_h, wm_y, info_x, info_y, line_h;
static int pads_x, pads_y, pad_size, pad_gap, bar_y, bar_h;

static gfx *g;
static const screen_info *info;
static int wm_x;

/* Scale a vertical or horizontal design value to the framebuffer */
static int sy(int v) { return v * g->h / DESIGN_H; }
static int sx(int v) { return v * g->w / DESIGN_W; }

static void layout_init(void)
{
    wm_h = sy(104);
    wm_y = sy(34);
    info_x = sx(36);
    info_y = sy(200);
    line_h = sy(20);
    pad_size = sy(52);
    pad_gap = sy(8);
    pads_y = sy(208);
    /* the pad grid keeps the same right margin as the text's left margin */
    pads_x = g->w - info_x - 4 * pad_size - 3 * pad_gap;
    bar_h = sy(24);
    bar_y = g->h - bar_h;
}

/* Wordmark shading: accent to warm white across, plus a moving highlight */
typedef struct wm_shade {
    int sweep;      /* x of the highlight, or a large negative value */
    int width;
} wm_shade;

static uint32_t wm_colour(int x, int y, void *user)
{
    wm_shade *s = user;
    uint32_t base = gfx_mix(ACCENT, RGB(255, 196, 120), x * 255 / s->width);
    int d = x + y / 3 - s->sweep;

    if (d > -24 && d < 24) {
        int a = 220 - (d < 0 ? -d : d) * 9;

        base = gfx_mix(base, RGB(255, 255, 255), a < 0 ? 0 : a);
    }
    return base;
}

static void background_rect(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++) {
        gfx_fill(g, x, j, w, 1, gfx_mix(BG_TOP, BG_BOTTOM,
                                        j * 255 / (g->h - 1)));
    }
}

static void background(int y, int h)
{
    background_rect(0, y, g->w, h);
}

static void draw_wordmark(uint64_t tick)
{
    int w = gfx_wordmark_width(wm_h);
    wm_shade shade = { -1000, w };
    int reveal;

    /* Intro: wipe in from the left over the first second */
    reveal = tick < 100 ? (int)(tick * (w + 40) / 100) : w + 40;
    if (tick >= 100) {
        shade.sweep = (int)((tick - 100) * 6 % (w + 600)) - 120;
    }
    background(wm_y - 4, wm_h + 8);
    gfx_wordmark(g, wm_x, wm_y, wm_h, wm_colour, &shade);
    if (reveal < w) {
        background_rect(wm_x + reveal, wm_y - 4, w - reveal + 8,
                        wm_h + 8);
    }
    gfx_flush(g, wm_y - 4, wm_h + 8);
}

static void draw_static(void)
{
    const char *sub = "bare metal on the emulated i.MX 8M Mini";

    background(0, g->h);
    gfx_text(g, (g->w - gfx_text_width(2, sub)) / 2, wm_y + wm_h + 16,
             2, TEXT_DIM, sub);
    gfx_fill(g, info_x, info_y - 12, pads_x - info_x - 40, 2, ACCENT);
    gfx_text(g, pads_x, pads_y - 18, 1, TEXT_DIM, "PADS (MCU LINK)");
    gfx_flush(g, 0, g->h);
}

static void line(int n, const char *label, const char *value)
{
    int y = info_y + n * line_h;

    int vx = info_x + 8 * 2 * 6;
    int room = pads_x - 24 - vx;

    gfx_text(g, info_x, y, 2, TEXT_DIM, label);
    /* long values drop to the small font so they fit */
    if (gfx_text_width(2, value) <= room) {
        gfx_text(g, vx, y, 2, TEXT, value);
    } else {
        gfx_text(g, vx, y + 4, 1, TEXT, value);
    }
}

static void draw_info(uint64_t tick)
{
    const panel_state *p = panel_get();
    char buf[48];
    int temp = soc_temperature();
    uint64_t s = tick / KICK_TICK_HZ;

    background_rect(info_x, info_y, pads_x - info_x - 24, 12 * line_h);
    line(0, "SoC", info->soc);
    line(1, "UID", info->uid);
    line(2, "CPUs", info->cpus);
    line(3, "Clock", info->clocks);
    line(4, "I2C", info->i2c);
    fmt_snprintf(buf, sizeof(buf), "%d C", temp);
    line(5, "Temp", temp < 0 ? "-" : buf);
    fmt_snprintf(buf, sizeof(buf), "%02u:%02u:%02u  %llu ticks",
                 (unsigned)(s / 3600), (unsigned)(s / 60 % 60),
                 (unsigned)(s % 60), (unsigned long long)tick);
    line(6, "Up", buf);
    fmt_snprintf(buf, sizeof(buf), "%lu IRQs, %u frames", gic_irq_count(),
                 lcdif_frames());
    line(7, "GIC", buf);
    fmt_snprintf(buf, sizeof(buf), "%s  tx %u rx %u",
                 p->linked ? "linked" : "waiting", p->frames_sent,
                 p->frames_received);
    line(8, "MCU", buf);
    if (p->last_pad) {
        fmt_snprintf(buf, sizeof(buf), "pad %d, velocity %d (%u hits)",
                     p->last_pad, p->last_velocity, p->pad_presses);
    } else {
        fmt_snprintf(buf, sizeof(buf), "hit a pad");
    }
    line(9, "Last", buf);
    gfx_flush(g, info_y, 12 * line_h);
}

static void draw_pads(void)
{
    const panel_state *p = panel_get();

    for (int pad = 0; pad < PANEL_PADS; pad++) {
        /* pad 1 is bottom left, as on the KICK */
        int x = pads_x + (pad % 4) * (pad_size + pad_gap);
        int y = pads_y + (3 - pad / 4) * (pad_size + pad_gap);
        uint32_t c = panel_colour_rgb(p->pad_colour[pad]);

        gfx_fill(g, x, y, pad_size, pad_size, gfx_mix(c, RGB(0, 0, 0), 120));
        gfx_fill(g, x + pad_gap / 2, y + pad_gap / 2, pad_size - pad_gap,
                 pad_size - pad_gap, c);
    }
    gfx_flush(g, pads_y, 4 * (pad_size + pad_gap));
}

static void draw_bar(uint64_t tick)
{
    for (int x = 0; x < g->w; x++) {
        int hue = (int)((x * 1536 / g->w + tick * 24) % 1536);

        gfx_fill(g, x, bar_y, 1, bar_h,
                 gfx_mix(gfx_hue(hue), BG_BOTTOM, 60));
    }
    gfx_flush(g, bar_y, bar_h);
}

void screen_init(gfx *gfx_ctx, const screen_info *i)
{
    g = gfx_ctx;
    info = i;
    layout_init();
    wm_x = (g->w - gfx_wordmark_width(wm_h)) / 2;
    draw_static();
}

void screen_update(uint64_t tick)
{
    static uint64_t last_info = (uint64_t)-1;

    draw_wordmark(tick);
    draw_bar(tick);
    if (tick < 100) {
        return;     /* intro: the wordmark first */
    }
    draw_pads();
    if (tick / 25 != last_info) {
        last_info = tick / 25;
        draw_info(tick);
    }
}
