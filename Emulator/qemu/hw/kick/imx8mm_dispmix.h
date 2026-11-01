/*
 * i.MX8M Mini DISPLAY_BLK_CTRL (display mix GPR, "dispmix")
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_DISPMIX_H
#define KICK_IMX8MM_DISPMIX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_DISPMIX "imx8mm-dispmix"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMDispMixState, IMX8MM_DISPMIX)

/* Size of the block as mapped by the DT (display-gpr@32e28000, reg size 0x100). */
#define IMX8MM_DISPMIX_MMIO_SIZE    0x100
#define IMX8MM_DISPMIX_NUM_REGS     (IMX8MM_DISPMIX_MMIO_SIZE / 4)

/* Reset/clock lines exported as GPIO outputs (one per register bit). */
#define IMX8MM_DISPMIX_NUM_SFT_RSTN 7
#define IMX8MM_DISPMIX_NUM_CLK_EN   13
#define IMX8MM_DISPMIX_NUM_MIPI_RST 2

struct IMX8MMDispMixState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX8MM_DISPMIX_NUM_REGS];

    qemu_irq sft_rstn[IMX8MM_DISPMIX_NUM_SFT_RSTN];
    qemu_irq clk_en[IMX8MM_DISPMIX_NUM_CLK_EN];
    qemu_irq mipi_rst[IMX8MM_DISPMIX_NUM_MIPI_RST];
};

#endif
