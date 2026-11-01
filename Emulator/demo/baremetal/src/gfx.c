/*
 * Software rendering into a 32 bpp XRGB8888 framebuffer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "font8x8_basic.h"
#include "gfx.h"
#include "mmu.h"

void gfx_init(gfx *g, uint32_t addr, int w, int h)
{
    g->fb = (uint32_t *)(uintptr_t)addr;
    g->w = w;
    g->h = h;
}

static int clip(gfx *g, int *x, int *y, int *w, int *h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > g->w) {
        *w = g->w - *x;
    }
    if (*y + *h > g->h) {
        *h = g->h - *y;
    }
    return *w > 0 && *h > 0;
}

void gfx_fill(gfx *g, int x, int y, int w, int h, uint32_t c)
{
    if (!clip(g, &x, &y, &w, &h)) {
        return;
    }
    for (int j = 0; j < h; j++) {
        uint32_t *p = g->fb + (y + j) * g->w + x;

        for (int i = 0; i < w; i++) {
            p[i] = c;
        }
    }
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int alpha)
{
    int ra = (a >> 16) & 0xff, ga = (a >> 8) & 0xff, ba = a & 0xff;
    int rb = (b >> 16) & 0xff, gb = (b >> 8) & 0xff, bb = b & 0xff;

    return RGB(ra + ((rb - ra) * alpha) / 255, ga + ((gb - ga) * alpha) / 255,
               ba + ((bb - ba) * alpha) / 255);
}

void gfx_vgradient(gfx *g, int x, int y, int w, int h, uint32_t top,
                   uint32_t bottom)
{
    for (int j = 0; j < h; j++) {
        gfx_fill(g, x, y + j, w, 1, gfx_mix(top, bottom, j * 255 / (h - 1)));
    }
}

uint32_t gfx_hue(int hue)
{
    int s = (hue / 256) % 6, f = hue % 256;

    switch (s) {
    case 0:
        return RGB(255, f, 0);
    case 1:
        return RGB(255 - f, 255, 0);
    case 2:
        return RGB(0, 255, f);
    case 3:
        return RGB(0, 255 - f, 255);
    case 4:
        return RGB(f, 0, 255);
    default:
        return RGB(255, 0, 255 - f);
    }
}

int gfx_text_width(int scale, const char *s)
{
    int n = 0;

    while (*s++) {
        n++;
    }
    return n * 8 * scale;
}

void gfx_text(gfx *g, int x, int y, int scale, uint32_t c, const char *s)
{
    for (; *s; s++, x += 8 * scale) {
        const unsigned char *glyph = font8x8_basic[(unsigned char)*s & 0x7f];

        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                /* bit 0 is the leftmost pixel */
                if (glyph[row] & (1u << col)) {
                    gfx_fill(g, x + col * scale, y + row * scale, scale,
                             scale, c);
                }
            }
        }
    }
}

/*
 * Wordmark geometry, all relative to the cap height H:
 * stroke H/5, K width 0.72 H, C is a ring of diameter H open to the right,
 * letter gap H/6.
 */
#define WM_STROKE(h)    ((h) / 5)
#define WM_K(h)         ((h) * 72 / 100)
#define WM_GAP(h)       ((h) / 6)

int gfx_wordmark_width(int h)
{
    return 2 * WM_K(h) + WM_STROKE(h) + h + 3 * WM_GAP(h);
}

typedef struct wm_ctx {
    gfx *g;
    int x0, y0;
    uint32_t (*shade)(int, int, void *);
    void *user;
} wm_ctx;

static void wm_plot(wm_ctx *c, int x, int y)
{
    int px = c->x0 + x, py = c->y0 + y;

    if (px >= 0 && py >= 0 && px < c->g->w && py < c->g->h) {
        c->g->fb[py * c->g->w + px] = c->shade(x, y, c->user);
    }
}

static void wm_span(wm_ctx *c, int x1, int x2, int y)
{
    for (int x = x1; x < x2; x++) {
        wm_plot(c, x, y);
    }
}

static void wm_letter_k(wm_ctx *c, int x, int h)
{
    int w = WM_STROKE(h), kw = WM_K(h);
    int reach = kw - w - w * 6 / 10;

    for (int y = 0; y < h; y++) {
        int d = y < h / 2 ? h / 2 - y : y - h / 2;
        int cx = w + d * reach / (h / 2);
        int hw = w * 62 / 100;
        int a = cx - hw < w / 2 ? w / 2 : cx - hw;
        int b = cx + hw > kw ? kw : cx + hw;

        wm_span(c, x, x + w, y);            /* stem */
        wm_span(c, x + a, x + b, y);        /* arms */
    }
}

static void wm_letter_i(wm_ctx *c, int x, int h)
{
    for (int y = 0; y < h; y++) {
        wm_span(c, x, x + WM_STROKE(h), y);
    }
}

static void wm_letter_c(wm_ctx *c, int x, int h)
{
    int r = h / 2, w = WM_STROKE(h);
    int r2 = r * r, ri2 = (r - w) * (r - w);

    for (int y = 0; y < h; y++) {
        for (int i = 0; i < h; i++) {
            /* pixel centre relative to the ring centre, in half pixels */
            int dx = 2 * i + 1 - h, dy = 2 * y + 1 - h;
            int d2 = (dx * dx + dy * dy) / 4;
            int ady = dy < 0 ? -dy : dy;

            if (d2 > r2 || d2 < ri2) {
                continue;
            }
            /* opening on the right: |dy| < 0.8 dx */
            if (dx > 0 && ady * 5 < dx * 4) {
                continue;
            }
            wm_plot(c, x + i, y);
        }
    }
}

int gfx_wordmark(gfx *g, int x, int y, int h,
                 uint32_t (*shade)(int, int, void *), void *user)
{
    wm_ctx c = { g, x, y, shade, user };
    int pos = 0;

    wm_letter_k(&c, pos, h);
    pos += WM_K(h) + WM_GAP(h);
    wm_letter_i(&c, pos, h);
    pos += WM_STROKE(h) + WM_GAP(h);
    wm_letter_c(&c, pos, h);
    pos += h + WM_GAP(h);
    wm_letter_k(&c, pos, h);
    return gfx_wordmark_width(h);
}

void gfx_flush(gfx *g, int y, int h)
{
    cache_clean_range(g->fb + y * g->w, (uint64_t)h * g->w * 4);
}
