/*
 * General Purpose Timer 1 as the periodic system tick
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_GPT_H
#define KICK_DEMO_GPT_H

#include <stdint.h>

/* Tick at @hz from a @clk_hz GPT clock root; IRQ through the GIC */
void gpt_start(uint32_t clk_hz, uint32_t hz);
uint64_t gpt_ticks(void);

#endif
