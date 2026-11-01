/*
 * Display bring-up the way ATF and the 5.4 kernel do it on the i.MX8MM:
 *
 * 1. GPC: power up the DISPMIX (PU domain 10) and MIPI (domain 0) domains
 *    with PU_PGC_UP_TRG and complete the ADB400 bus handshake
 *    (GPC_PU_PWRHSK DISPMIX sync bit 7, acknowledge bit 25).
 * 2. CCM: enable the display clock gate, set DISP_AXI/DISP_APB and the
 *    LCDIF pixel clock root.
 * 3. DISPLAY_BLK_CTRL (0x32e28000): release the soft resets and enable
 *    the block clocks, as ATF imx_gpc_pm_domain_enable() does for DISPMIX.
 * 4. eLCDIF (IMX8MMRM chapter 13.4): soft reset, DOTCLK master mode,
 *    24-bit bus, 32 bpp input (BYTE_PACKING 0x7), timings, frame buffer,
 *    RUN.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "clock.h"
#include "gic.h"
#include "imx8mm.h"
#include "lcdif.h"

/* GPC */
#define GPC_PU_PGC_UP_TRG       0xf8
#define GPC_PU_PWRHSK           0x1fc
#define GPC_PGC(n)              (0xc00 + (n) * 0x40)
#define PU_MIPI                 0
#define PU_DISPMIX              10
#define PWRHSK_DISPMIX_SYNC     (1u << 7)
#define PWRHSK_DISPMIX_ACK      (1u << 25)

/* eLCDIF */
#define CTRL                    0x000
#define CTRL1                   0x010
#define TRANSFER_COUNT          0x030
#define CUR_BUF                 0x040
#define NEXT_BUF                0x050
#define VDCTRL0                 0x070
#define VDCTRL1                 0x080
#define VDCTRL2                 0x090
#define VDCTRL3                 0x0a0
#define VDCTRL4                 0x0b0

#define CTRL_SFTRST             (1u << 31)
#define CTRL_CLKGATE            (1u << 30)
#define CTRL_BYPASS_COUNT       (1u << 19)
#define CTRL_DOTCLK_MODE        (1u << 17)
#define CTRL_BUS_WIDTH_24       (3u << 10)
#define CTRL_WORD_LENGTH_24     (3u << 8)
#define CTRL_MASTER             (1u << 5)
#define CTRL_RUN                (1u << 0)
#define CTRL1_BYTE_PACKING_32   (0x7u << 16)
#define CTRL1_CUR_FRAME_DONE    (1u << 9)
#define CTRL1_CUR_FRAME_DONE_EN (1u << 13)
#define VDCTRL0_ENABLE_PRESENT  (1u << 28)
#define VDCTRL0_ENABLE_POL      (1u << 24)
#define VDCTRL0_PERIOD_UNIT     (1u << 21)
#define VDCTRL0_PULSE_UNIT      (1u << 20)
#define VDCTRL4_SYNC_SIGNALS_ON (1u << 18)

static volatile uint32_t frames;

static void lcdif_irq(void)
{
    if (rd32(LCDIF_BASE + CTRL1) & CTRL1_CUR_FRAME_DONE) {
        wr32(LCDIF_BASE + CTRL1 + REG_CLR, CTRL1_CUR_FRAME_DONE);
        frames++;
    }
}

uint32_t lcdif_frames(void)
{
    return frames;
}

static void power_up_dispmix(void)
{
    wr32(GPC_BASE + GPC_PGC(PU_MIPI), 0);
    wr32(GPC_BASE + GPC_PGC(PU_DISPMIX), 0);
    wr32(GPC_BASE + GPC_PU_PGC_UP_TRG, (1u << PU_MIPI) | (1u << PU_DISPMIX));
    while (rd32(GPC_BASE + GPC_PU_PGC_UP_TRG)) {
    }
    wr32(GPC_BASE + GPC_PU_PWRHSK,
         rd32(GPC_BASE + GPC_PU_PWRHSK) | PWRHSK_DISPMIX_SYNC);
    while (!(rd32(GPC_BASE + GPC_PU_PWRHSK) & PWRHSK_DISPMIX_ACK)) {
    }
}

void lcdif_start(const lcdif_mode *m, uint32_t fb_addr)
{
    uint32_t htotal = m->hactive + m->hfp + m->hsync + m->hbp;
    uint32_t vtotal = m->vactive + m->vfp + m->vsync + m->vbp;

    power_up_dispmix();

    /*
     * Clocks: DISP_AXI and DISP_APB from SYS_PLL2_1000M / SYS_PLL1_800M,
     * pixel clock from VIDEO_PLL1 (594 MHz) / 3 / 7 = 28.3 MHz, close to
     * the 29.2 MHz an 800x480 panel needs for 60 Hz with these porches.
     */
    clock_gate(CCGR_DISPMIX, 1);
    clock_root(ROOT_DISP_AXI, 1, 1, 2);
    clock_root(ROOT_DISP_APB, 1, 1, 4);
    clock_root(ROOT_LCDIF_PIXEL, 1, 3, 7);

    /* Display block control: clocks on, then out of soft reset */
    wr32(DISPMIX_BASE + 0x4, 0x1fff);
    wr32(DISPMIX_BASE + 0x0, 0x7f);
    wr32(DISPMIX_BASE + 0x8, 0x30000);

    /* eLCDIF soft reset: SFTRST then CLKGATE are cleared to leave reset */
    wr32(LCDIF_BASE + CTRL + REG_CLR, CTRL_SFTRST);
    wr32(LCDIF_BASE + CTRL + REG_CLR, CTRL_CLKGATE);

    wr32(LCDIF_BASE + CTRL, CTRL_BYPASS_COUNT | CTRL_DOTCLK_MODE |
                            CTRL_BUS_WIDTH_24 | CTRL_WORD_LENGTH_24 |
                            CTRL_MASTER);
    wr32(LCDIF_BASE + CTRL1, CTRL1_BYTE_PACKING_32 | CTRL1_CUR_FRAME_DONE_EN);
    wr32(LCDIF_BASE + TRANSFER_COUNT,
         ((uint32_t)m->vactive << 16) | m->hactive);
    wr32(LCDIF_BASE + VDCTRL0, VDCTRL0_ENABLE_PRESENT | VDCTRL0_ENABLE_POL |
                               VDCTRL0_PERIOD_UNIT | VDCTRL0_PULSE_UNIT |
                               m->vsync);
    wr32(LCDIF_BASE + VDCTRL1, vtotal);
    wr32(LCDIF_BASE + VDCTRL2, ((uint32_t)m->hsync << 18) | htotal);
    wr32(LCDIF_BASE + VDCTRL3, ((uint32_t)(m->hsync + m->hbp) << 16) |
                               (m->vsync + m->vbp));
    wr32(LCDIF_BASE + VDCTRL4, VDCTRL4_SYNC_SIGNALS_ON | m->hactive);
    wr32(LCDIF_BASE + CUR_BUF, fb_addr);
    wr32(LCDIF_BASE + NEXT_BUF, fb_addr);

    gic_enable_spi(IRQ_LCDIF, 0xa0, lcdif_irq);
    wr32(LCDIF_BASE + CTRL + REG_SET, CTRL_RUN);
}
