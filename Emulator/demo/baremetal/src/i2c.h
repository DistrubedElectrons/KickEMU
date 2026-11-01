/*
 * i.MX I2C controller (master, polled)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_I2C_H
#define KICK_DEMO_I2C_H

#include <stdint.h>

/* Bus numbers are 1..4 as in the RM (I2C1 = Linux i2c-0) */
void i2c_init(int bus);
/* Address-only probe: returns 1 if a device ACKs @addr */
int i2c_probe(int bus, uint8_t addr);
/* Register read: write @reg, repeated start, read @len bytes */
int i2c_read_reg(int bus, uint8_t addr, uint8_t reg, uint8_t *buf, int len);

#endif
