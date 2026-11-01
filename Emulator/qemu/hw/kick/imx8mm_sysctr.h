/*
 * i.MX 8M Mini System Counter (SYS_CTR)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_SYSCTR_H
#define KICK_IMX8MM_SYSCTR_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_IMX8MM_SYSCTR "imx8mm.sysctr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMSysCtrState, IMX8MM_SYSCTR)

#define IMX8MM_SYSCTR_FRAME_SIZE    0x10000
#define IMX8MM_SYSCTR_NUM_CMP       2

typedef struct IMX8MMSysCtrCmp {
    uint64_t cmpcv;
    uint32_t cmpcr;
    QEMUTimer *timer;
    qemu_irq irq;
} IMX8MMSysCtrCmp;

struct IMX8MMSysCtrState {
    SysBusDevice parent_obj;

    /* MMIO 0: read frame, 1: compare frame, 2: control frame */
    MemoryRegion iomem_rd;
    MemoryRegion iomem_cmp;
    MemoryRegion iomem_ctrl;

    uint32_t cntcr;
    /* Counter value at base_ns; runs at the selected frequency when EN=1 */
    uint64_t base_cnt;
    int64_t base_ns;

    IMX8MMSysCtrCmp cmp[IMX8MM_SYSCTR_NUM_CMP];

    /* Properties */
    uint32_t fid[2];
    bool start_enabled;
};

#endif /* KICK_IMX8MM_SYSCTR_H */
