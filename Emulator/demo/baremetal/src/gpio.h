/*
 * i.MX GPIO (GPIO1..GPIO5, 32 pins each)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_GPIO_H
#define KICK_DEMO_GPIO_H

void gpio_set_output(int bank, int pin, int value);
void gpio_set(int bank, int pin, int value);
/* Pad state as seen by the PSR (pad status) register */
int gpio_get(int bank, int pin);

#endif
