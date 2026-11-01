/*
 * NXP PCA9534 8-bit I2C GPIO expander with interrupt output
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_PCA9534_H
#define HW_KICK_PCA9534_H

#define TYPE_PCA9534 "pca9534"

/*
 * GPIO interface (qdev GPIOs):
 *   in  (unnamed) 0..7  level applied to the pin from outside
 *   out (unnamed) 0..7  resulting pin level (output value or outside level)
 *   out "int"           INT pin, open drain, active low
 * QOM property "input-levels" (uint8, settable at run time with qom-set)
 * sets the outside levels of all pins at once, e.g. for host side buttons.
 */
#define PCA9534_GPIO_INT    "int"

#endif
