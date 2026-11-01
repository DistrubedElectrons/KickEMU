/*
 * GICv3 driver for CPU 0, non-secure EL1 (single security state)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_GIC_H
#define KICK_DEMO_GIC_H

#include <stdint.h>

typedef void (*irq_fn)(void);

void gic_init(void);
/* Route shared interrupt @intid to CPU 0 at @prio and call @fn for it */
void gic_enable_spi(unsigned intid, uint8_t prio, irq_fn fn);
void irq_enable(void);
void irq_disable(void);
unsigned long gic_irq_count(void);

#endif
