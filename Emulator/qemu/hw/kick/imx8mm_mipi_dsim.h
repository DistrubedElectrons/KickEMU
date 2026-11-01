/*
 * i.MX8M Mini MIPI DSI host controller (Samsung DSIM)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_MIPI_DSIM_H
#define KICK_IMX8MM_MIPI_DSIM_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_IMX8MM_MIPI_DSIM "imx8mm-mipi-dsim"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMMipiDsimState, IMX8MM_MIPI_DSIM)

/* The DT maps 1 KiB (mipi_dsi@32e10000, reg size 0x400). */
#define IMX8MM_DSIM_MMIO_SIZE       0x400
#define IMX8MM_DSIM_NUM_REGS        (IMX8MM_DSIM_MMIO_SIZE / 4)

/* SFR FIFO depths of the DSIM (header entries, payload and RX bytes). */
#define IMX8MM_DSIM_PH_FIFO_DEPTH   16
#define IMX8MM_DSIM_PL_FIFO_BYTES   2048
#define IMX8MM_DSIM_RX_FIFO_BYTES   256

struct IMX8MMMipiDsimState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    Clock *pll_ref;
    QEMUTimer *pll_timer;
    Object *peripheral;

    uint32_t regs[IMX8MM_DSIM_NUM_REGS];
    Fifo8 pl_fifo;              /* SFR payload FIFO */
    Fifo8 rx_fifo;              /* RX FIFO, read as 32-bit words */
    bool pll_stable;
    bool in_reset;              /* held by the dispmix reset line */

    /* Properties. */
    uint32_t version;
    uint32_t fallback_pll_ref_hz;
};

/*
 * True while the D-PHY high-speed clock lane runs (PLL locked and
 * CLKCTRL.TXREQUESTHSCLK set). Used by downstream bridges whose PLL is
 * clocked from the DSI clock.
 */
bool imx8mm_dsim_hs_clock_running(IMX8MMMipiDsimState *s);

/* True while the main display path streams video (MDRESOL.MAINSTANDBY). */
bool imx8mm_dsim_video_active(IMX8MMMipiDsimState *s);

#endif
