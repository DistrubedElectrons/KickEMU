/*
 * On-screen intro and live status
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_SCREEN_H
#define KICK_DEMO_SCREEN_H

#include <stdint.h>

#include "gfx.h"

/* Static facts shown on the screen, gathered once at boot */
typedef struct screen_info {
    char soc[48];
    char uid[32];
    char cpus[32];
    char clocks[48];
    char i2c[64];
} screen_info;

void screen_init(gfx *g, const screen_info *info);
/* Redraw the animated parts for GPT tick @tick */
void screen_update(uint64_t tick);

#endif
