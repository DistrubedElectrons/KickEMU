/*
 * Analog Devices AD5671R/AD5675R octal I2C nanoDAC+ (KICK CV/Gate outputs)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_AD5675R_H
#define HW_KICK_AD5675R_H

/* One model for both parts, selected by the "resolution" property */
#define TYPE_AD5675R "ad5675r"

/*
 * GPIO interface (qdev named GPIOs):
 *   in "ldac"   LDAC pin, active low; while low, input registers of channels
 *               not masked in the LDAC mask register update their DAC
 *   in "reset"  RESET pin, active low
 */
#define AD5675R_GPIO_LDAC   "ldac"
#define AD5675R_GPIO_RESET  "reset"

#endif
