/*
 * Front panel light show over the MCU link (UART1)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_PANEL_H
#define KICK_DEMO_PANEL_H

#include <stdint.h>

#define PANEL_PADS      16

typedef struct panel_state {
    int linked;                 /* MCU answered the handshake */
    uint8_t pad_colour[PANEL_PADS];   /* PadColourIds last sent */
    int last_pad;               /* 1..16, 0 = none yet */
    int last_velocity;
    int last_button;            /* BtnLedSequence id, -1 = none */
    unsigned pad_presses;
    unsigned frames_sent;
    unsigned frames_received;
} panel_state;

void panel_init(void);
/* Feed bytes received on UART1 (call from the main loop) */
void panel_rx(uint8_t byte);
/* Advance the animation; @tick counts GPT ticks */
void panel_animate(uint64_t tick);
const panel_state *panel_get(void);
/* Approximate RGB of a pad colour id for the on-screen mirror */
uint32_t panel_colour_rgb(int id);

#endif
