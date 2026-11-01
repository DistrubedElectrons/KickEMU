/*
 * i.MX8M Mini eLCDIF display controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_LCDIF_H
#define KICK_IMX8MM_LCDIF_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_IMX8MM_LCDIF "imx8mm-lcdif"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMLCDIFState, IMX8MM_LCDIF)

/* The DT maps 64 KiB (lcdif@32e00000, reg size 0x10000). */
#define IMX8MM_LCDIF_MMIO_SIZE      0x10000
/* Registers up to and including the PIGEON block end below 0xc00. */
#define IMX8MM_LCDIF_REG_SPACE      0xc00
#define IMX8MM_LCDIF_NUM_REGS       (IMX8MM_LCDIF_REG_SPACE / 0x10)

struct IMX8MMLCDIFState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    QemuConsole *con;
    Clock *pix_clk;
    QEMUTimer *frame_timer;
    qemu_irq irq;

    /* One 32-bit register per 16-byte slot (base, SET, CLR, TOG). */
    uint32_t regs[IMX8MM_LCDIF_NUM_REGS];

    /* Frame timing state. */
    bool frame_active;          /* between VSYNC edge and CUR_FRAME_DONE */
    int64_t frame_start_ns;

    /* Scanout state derived from the registers at the last update. */
    MemoryRegionSection fbsection;
    hwaddr fb_base;
    uint32_t fb_width;          /* pixels fetched per line (TRANSFER_COUNT) */
    uint32_t fb_height;
    uint32_t out_width;         /* visible width after PIGEON cropping */
    uint32_t src_stride;
    int src_format;
    bool invalidate;

    /* Properties. */
    uint32_t panel_width;
    uint32_t panel_height;
    uint32_t fallback_refresh_hz;
};

#endif
