/*
 * i.MX Smart DMA controller (SDMA v2, i.MX 8M Mini)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_IMX_SDMA_H
#define HW_KICK_IMX_SDMA_H

#include "system/memory.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX_SDMA "imx8mm-sdma"
OBJECT_DECLARE_SIMPLE_TYPE(IMXSDMAState, IMX_SDMA)

#define IMX_SDMA_MMIO_SIZE      0x10000
#define IMX_SDMA_CHANNELS       32
#define IMX_SDMA_EVENTS         48

/*
 * Inputs (qdev named GPIOs): "events", IMX_SDMA_EVENTS lines, the DMA
 * request inputs (level). The interrupt is sysbus IRQ 0.
 */
#define IMX_SDMA_GPIO_EVENTS    "events"

typedef struct IMXSDMAChannel {
    bool fresh;         /* context (re)loaded: start at the context PC */
    bool active;        /* a script is executing for this channel */
    bool blocked;       /* waiting for the CPU to hand over the next BD */
    uint32_t pc;
    uint32_t script;    /* index into the script table */
    uint32_t bd;        /* current buffer descriptor address */
    uint32_t bd_bytes;  /* bytes already transferred for the current BD */
} IMXSDMAChannel;

struct IMXSDMAState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUBH *bh;
    QEMUTimer *poll_timer;
    MemoryRegion *dma_mr;       /* "dma-mr" link: bus master view */
    AddressSpace *dma_as;

    /* Properties */
    uint32_t ram_size;
    uint32_t bd_poll_us;

    /* Host registers */
    uint32_t mc0ptr;
    uint32_t intr;
    uint32_t he;            /* HSTART/STOP_STAT: host enable per channel */
    uint32_t evtovr;
    uint32_t dspovr;
    uint32_t hostovr;
    uint32_t evtpend;
    uint32_t dspenbl;
    uint32_t evterr;
    uint32_t intrmask;
    uint32_t psw;
    uint32_t config;
    uint32_t lock;
    uint32_t once_enb;
    uint32_t once_data;
    uint32_t once_instr;
    uint32_t once_stat;
    uint32_t once_cmd;
    uint32_t illinstaddr;
    uint32_t chn0addr;
    uint32_t xtrig_conf[2];
    uint32_t chnpri[IMX_SDMA_CHANNELS];
    uint32_t chnenbl[IMX_SDMA_EVENTS];

    /* Event input levels */
    uint64_t evt_level;

    /* SDMA internal RAM, 32 bit data memory words starting at DM 0x800 */
    uint32_t *ram;
    uint32_t ram_words;

    IMXSDMAChannel chan[IMX_SDMA_CHANNELS];
    bool in_run;
};

#endif
