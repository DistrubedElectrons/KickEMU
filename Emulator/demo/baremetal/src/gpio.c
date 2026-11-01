/*
 * i.MX GPIO (IMX8MMRM chapter 8.3): DR 0x00 data, GDIR 0x04 direction
 * (1 = output), PSR 0x08 pad status. A DR read returns the written value
 * for outputs and the pad state (PSR) for inputs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gpio.h"
#include "imx8mm.h"

#define GPIO_DR     0x00
#define GPIO_GDIR   0x04

static uint32_t gpio_base(int bank)
{
    return GPIO1_BASE + (uint32_t)(bank - 1) * 0x10000;
}

void gpio_set(int bank, int pin, int value)
{
    uint32_t b = gpio_base(bank);
    uint32_t dr = rd32(b + GPIO_DR);

    wr32(b + GPIO_DR, value ? dr | (1u << pin) : dr & ~(1u << pin));
}

void gpio_set_output(int bank, int pin, int value)
{
    uint32_t b = gpio_base(bank);

    gpio_set(bank, pin, value);
    wr32(b + GPIO_GDIR, rd32(b + GPIO_GDIR) | (1u << pin));
}

int gpio_get(int bank, int pin)
{
    return (rd32(gpio_base(bank) + GPIO_DR) >> pin) & 1;
}
