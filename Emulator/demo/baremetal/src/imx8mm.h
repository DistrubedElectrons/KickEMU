/*
 * i.MX 8M Mini register map used by the demo (IMX8MMRM chapter 2).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_IMX8MM_H
#define KICK_DEMO_IMX8MM_H

#include <stdint.h>

#define GPIO1_BASE          0x30200000u     /* GPIOn at + (n - 1) * 0x10000 */
#define TMU_BASE            0x30260000u
#define GPT1_BASE           0x302d0000u
#define OCOTP_BASE          0x30350000u
#define ANATOP_BASE         0x30360000u
#define CCM_BASE            0x30380000u
#define GPC_BASE            0x303a0000u
#define UART1_BASE          0x30860000u
#define UART4_BASE          0x30a60000u
#define I2C1_BASE           0x30a20000u     /* I2Cn at + (n - 1) * 0x10000 */
#define LCDIF_BASE          0x32e00000u
#define DISPMIX_BASE        0x32e28000u
#define GICD_BASE           0x38800000u
#define GICR_BASE           0x38880000u     /* CPU 0 redistributor */

/* Shared peripheral interrupts (GIC INTID = SPI + 32) */
#define IRQ_LCDIF           (32 + 5)
#define IRQ_UART1           (32 + 26)
#define IRQ_UART4           (32 + 29)
#define IRQ_GPT1            (32 + 55)

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void wr32(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static inline uint16_t rd16(uint32_t addr)
{
    return *(volatile uint16_t *)(uintptr_t)addr;
}

static inline void wr16(uint32_t addr, uint16_t val)
{
    *(volatile uint16_t *)(uintptr_t)addr = val;
}

/* The many i.MX registers with SET/CLR/TOG aliases at +4/+8/+0xc */
#define REG_SET             0x4
#define REG_CLR             0x8

#define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))

#endif
