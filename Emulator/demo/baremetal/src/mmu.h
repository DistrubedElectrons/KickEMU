/*
 * Identity mapping with caches: peripherals as Device, DDR as Normal memory
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_MMU_H
#define KICK_DEMO_MMU_H

#include <stdint.h>

void mmu_enable(void);
/* Write back a buffer the DMA masters (LCDIF) read from memory */
void cache_clean_range(const void *start, uint64_t len);

#endif
