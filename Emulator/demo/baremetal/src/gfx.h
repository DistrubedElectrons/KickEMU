/*
 * Software rendering into a 32 bpp XRGB8888 framebuffer
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_GFX_H
#define KICK_DEMO_GFX_H

#include <stdint.h>

typedef struct gfx {
    uint32_t *fb;
    int w, h;
} gfx;

#define RGB(r, g, b)    (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (b))

void gfx_init(gfx *g, uint32_t addr, int w, int h);
void gfx_fill(gfx *g, int x, int y, int w, int h, uint32_t c);
/* Vertical gradient from @top to @bottom */
void gfx_vgradient(gfx *g, int x, int y, int w, int h, uint32_t top,
                   uint32_t bottom);
/* 8x8 font scaled by @scale; background is left untouched */
void gfx_text(gfx *g, int x, int y, int scale, uint32_t c, const char *s);
int gfx_text_width(int scale, const char *s);
/* Blend @c over the pixel with @alpha/255 */
uint32_t gfx_mix(uint32_t a, uint32_t b, int alpha);
/* Hue 0..1535 (six 256-step sextants) at full saturation */
uint32_t gfx_hue(int hue);
/*
 * The "KICK" wordmark, @height pixels tall, starting at @x/@y. Colour
 * along x comes from @shade(x_offset, user) so callers can animate it.
 * Returns the width.
 */
int gfx_wordmark(gfx *g, int x, int y, int height,
                 uint32_t (*shade)(int xoff, int yoff, void *user),
                 void *user);
int gfx_wordmark_width(int height);
/* Write the framebuffer region back for the LCDIF DMA */
void gfx_flush(gfx *g, int y, int h);

#endif
