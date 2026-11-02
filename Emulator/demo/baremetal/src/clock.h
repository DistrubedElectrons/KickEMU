/*
 * CCM / CCM_ANALOG helpers: clock roots, gates and PLL rates
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_CLOCK_H
#define KICK_DEMO_CLOCK_H

#include <stdint.h>

#define OSC_24M_HZ          24000000u

/* CCM_TARGET_ROOT offsets (IMX8MMRM table 5-1) */
#define ROOT_DISP_AXI       0x8a00
#define ROOT_DISP_APB       0x8a80
#define ROOT_LCDIF_PIXEL    0xa500
#define ROOT_UART1          0xaf00
#define ROOT_UART4          0xb080
#define ROOT_GPT1           0xb580
#define ROOT_PWM1           0xb380

/* CCM_CCGR indices */
#define CCGR_GPT1           16
#define CCGR_I2C1           23      /* I2C1..I2C4 are 23..26 */
#define CCGR_PWM1           40
#define CCGR_UART1          73
#define CCGR_UART4          76
#define CCGR_DISPMIX        93

/* Program a clock root: input @mux, pre divider @pre (1..8), post @post */
void clock_root(uint32_t root, unsigned mux, unsigned pre, unsigned post);
void clock_gate(unsigned ccgr, int on);
/* Output frequency of a PLL from its GNRL_CTL offset in CCM_ANALOG */
uint64_t clock_pll_hz(uint32_t gnrl_off, int fractional);

#define ANA_ARM_PLL         0x84
#define ANA_SYS_PLL1        0x94
#define ANA_VIDEO_PLL1      0x28

#endif
