/*
 * Goodix GT911 capacitive touch controller (I2C slave)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_GOODIX_GT911_H
#define HW_KICK_GOODIX_GT911_H

#define TYPE_GOODIX_GT911 "goodix-gt911"

/*
 * GPIO interface (qdev named GPIOs):
 *   out "int"        INT pin driven by the controller, connect to the SoC GPIO input
 *   in  "int-sense"  level the SoC drives onto INT while it is an output (address select)
 *   in  "reset"      RSTB pin, active low (0 = held in reset)
 */
#define GT911_GPIO_INT          "int"
#define GT911_GPIO_INT_SENSE    "int-sense"
#define GT911_GPIO_RESET        "reset"

#endif
