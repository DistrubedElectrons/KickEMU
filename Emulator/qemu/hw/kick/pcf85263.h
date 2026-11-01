/*
 * NXP PCF85263A real-time clock (I2C slave)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_PCF85263_H
#define HW_KICK_PCF85263_H

#define TYPE_PCF85263 "pcf85263"

/*
 * GPIO interface (qdev named GPIOs):
 *   out "inta"  INTA pin (Pin_IO.INTAPM = 10), active low
 *   out "intb"  TS/INTB pin (Pin_IO.TSPM = 10), active low
 */
#define PCF85263_GPIO_INTA  "inta"
#define PCF85263_GPIO_INTB  "intb"

#endif
