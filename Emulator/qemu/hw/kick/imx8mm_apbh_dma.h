/*
 * i.MX 8M Mini APBH-Bridge-DMA (GPMI DMA): control registers only
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_IMX8MM_APBH_DMA_H
#define HW_KICK_IMX8MM_APBH_DMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_APBH_DMA "imx8mm.apbh-dma"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMApbhDmaState, IMX8MM_APBH_DMA)

#define IMX8MM_APBH_DMA_SIZE    0x2000

struct IMX8MMApbhDmaState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX8MM_APBH_DMA_SIZE / 16];   /* one per 16-byte slot */
};

#endif
