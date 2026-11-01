/*
 * eLCDIF scan-out of a 32 bpp framebuffer in DDR
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_LCDIF_H
#define KICK_DEMO_LCDIF_H

#include <stdint.h>

typedef struct lcdif_mode {
    uint16_t hactive, hfp, hsync, hbp;
    uint16_t vactive, vfp, vsync, vbp;
} lcdif_mode;

/* Power the display mix, clock the pixel root, start DOTCLK scan-out */
void lcdif_start(const lcdif_mode *m, uint32_t fb_addr);
/* Frame counter, incremented by the frame-done interrupt */
uint32_t lcdif_frames(void);

#endif
