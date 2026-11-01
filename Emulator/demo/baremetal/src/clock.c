/*
 * CCM / CCM_ANALOG helpers (IMX8MMRM chapter 5).
 *
 * CCM_TARGET_ROOTn: ENABLE [28], MUX [26:24], PRE_PODF [18:16],
 * POST_PODF [5:0]. CCM_CCGRn: 2-bit "needed" settings per domain, 0x3 in
 * domain 0 keeps the clock on while the A53 runs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "clock.h"
#include "imx8mm.h"

#define CCGR(n)             (0x4000 + (n) * 0x10)

void clock_root(uint32_t root, unsigned mux, unsigned pre, unsigned post)
{
    wr32(CCM_BASE + root, (1u << 28) | ((mux & 7) << 24) |
                          (((pre - 1) & 7) << 16) | ((post - 1) & 0x3f));
}

void clock_gate(unsigned ccgr, int on)
{
    wr32(CCM_BASE + CCGR(ccgr) + (on ? REG_SET : REG_CLR), 0x3);
}

/*
 * PLL14xx: Fout = 24 MHz * (MDIV + KDIV / 65536) / (PDIV * 2^SDIV), with
 * DIV_CTL0 = MDIV [21:12], PDIV [9:4], SDIV [2:0] and, for the fractional
 * PLL1443X, KDIV in DIV_CTL1 [15:0]. LOCK is GNRL_CTL bit 31.
 */
uint64_t clock_pll_hz(uint32_t gnrl_off, int fractional)
{
    uint32_t gnrl = rd32(ANATOP_BASE + gnrl_off);
    uint32_t div = rd32(ANATOP_BASE + gnrl_off + 4);
    uint64_t mdiv = (div >> 12) & 0x3ff;
    uint64_t pdiv = (div >> 4) & 0x3f;
    uint64_t sdiv = div & 7;
    int64_t kdiv = fractional ?
        (int16_t)(rd32(ANATOP_BASE + gnrl_off + 8) & 0xffff) : 0;

    if (!(gnrl & (1u << 31)) || !pdiv) {
        return 0;       /* not locked */
    }
    return ((uint64_t)OSC_24M_HZ * (mdiv * 65536 + kdiv) / (pdiv * 65536))
           >> sdiv;
}
