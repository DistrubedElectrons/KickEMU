/*
 * On-screen intro and live status, 800x480 layout:
 *
 *   y   0..175  wordmark (wipes in during the first second, then a light
 *               sweep runs across it), subtitle
 *   y 196..440  left: system facts and live counters; right: a mirror of
 *               the 4x4 pads as sent to the MCU
 *   y 452..479  animated rainbow bar
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

#define WM_HEIGHT       104
#define WM_Y            34
#define INFO_X          36
#define INFO_Y          200
#define LINE_H          20
#define PADS_X          548
#define PADS_Y          208
#define PAD_SIZE        52
#define PAD_GAP         8
#define BAR_Y           456
#define BAR_H           24

static gfx *g;
static const screen_info *info;
static int wm_x;

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
    int w = gfx_wordmark_width(WM_HEIGHT);
    wm_shade shade = { -1000, w };
    int reveal;

    /* Intro: wipe in from the left over the first second */
    reveal = tick < 100 ? (int)(tick * (w + 40) / 100) : w + 40;
    if (tick >= 100) {
        shade.sweep = (int)((tick - 100) * 6 % (w + 600)) - 120;
    }
    background(WM_Y - 4, WM_HEIGHT + 8);
    gfx_wordmark(g, wm_x, WM_Y, WM_HEIGHT, wm_colour, &shade);
    if (reveal < w) {
        background_rect(wm_x + reveal, WM_Y - 4, w - reveal + 8,
                        WM_HEIGHT + 8);
    }
    gfx_flush(g, WM_Y - 4, WM_HEIGHT + 8);
}

static void draw_static(void)
{
    const char *sub = "bare metal on the emulated i.MX 8M Mini";

    background(0, g->h);
    gfx_text(g, (g->w - gfx_text_width(2, sub)) / 2, WM_Y + WM_HEIGHT + 16,
             2, TEXT_DIM, sub);
    gfx_fill(g, INFO_X, INFO_Y - 12, PADS_X - INFO_X - 40, 2, ACCENT);
    gfx_text(g, PADS_X, INFO_Y - 26, 1, TEXT_DIM, "PADS (MCU LINK)");
    gfx_flush(g, 0, g->h);
}

static void line(int n, const char *label, const char *value)
{
    int y = INFO_Y + n * LINE_H;

    int vx = INFO_X + 8 * 2 * 6;
    int room = PADS_X - 24 - vx;

    gfx_text(g, INFO_X, y, 2, TEXT_DIM, label);
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

    background_rect(INFO_X, INFO_Y, PADS_X - INFO_X - 24, 12 * LINE_H);
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
    gfx_flush(g, INFO_Y, 12 * LINE_H);
}

static void draw_pads(void)
{
    const panel_state *p = panel_get();

    for (int pad = 0; pad < PANEL_PADS; pad++) {
        /* pad 1 is bottom left, as on the KICK */
        int x = PADS_X + (pad % 4) * (PAD_SIZE + PAD_GAP);
        int y = PADS_Y + (3 - pad / 4) * (PAD_SIZE + PAD_GAP);
        uint32_t c = panel_colour_rgb(p->pad_colour[pad]);

        gfx_fill(g, x, y, PAD_SIZE, PAD_SIZE, gfx_mix(c, RGB(0, 0, 0), 120));
        gfx_fill(g, x + 4, y + 4, PAD_SIZE - 8, PAD_SIZE - 8, c);
    }
    gfx_flush(g, PADS_Y, 4 * (PAD_SIZE + PAD_GAP));
}

static void draw_bar(uint64_t tick)
{
    for (int x = 0; x < g->w; x++) {
        int hue = (int)((x * 1536 / g->w + tick * 24) % 1536);

        gfx_fill(g, x, BAR_Y, 1, BAR_H,
                 gfx_mix(gfx_hue(hue), BG_BOTTOM, 60));
    }
    gfx_flush(g, BAR_Y, BAR_H);
}

void screen_init(gfx *gfx_ctx, const screen_info *i)
{
    g = gfx_ctx;
    info = i;
    wm_x = (g->w - gfx_wordmark_width(WM_HEIGHT)) / 2;
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
