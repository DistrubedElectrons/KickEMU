/*
 * Vivante GPU core (GCNanoUltra / GC520L of the i.MX8M Mini)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_VIVANTE_GPU_H
#define KICK_VIVANTE_GPU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_VIVANTE_GPU "vivante-gpu"
OBJECT_DECLARE_SIMPLE_TYPE(VivGpuState, VIVANTE_GPU)

/* i.MX8MM instances with the identification of the real cores. */
#define TYPE_IMX8MM_GPU3D "imx8mm-gpu3d"
#define TYPE_IMX8MM_GPU2D "imx8mm-gpu2d"

/* Each core decodes 32 KiB of AHB register space (DT: iobase_3d/_2d). */
#define VIV_MMIO_SIZE           0x8000
#define VIV_NUM_REGS            (0x1000 / 4)    /* HI/MC/FE/MMU registers */
/* LOAD_STATE addresses are 16-bit word indices. */
#define VIV_NUM_STATES          0x10000

/* MMUv2 exception codes (state_hi.xml MMU_EXCEPTION). */
#define VIV_MMU_SLAVE_NOT_PRESENT   1
#define VIV_MMU_PAGE_NOT_PRESENT    2
#define VIV_MMU_WRITE_VIOLATION     3

/* MMUv2 units used for exception reporting. */
#define VIV_MMU_UNIT_FE             0
#define VIV_MMU_UNIT_PE             1

#define VIV_PIPE_3D             0
#define VIV_PIPE_2D             1

struct VivGpuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    qemu_irq irq;
    QEMUTimer *fe_timer;

    uint32_t regs[VIV_NUM_REGS];
    uint32_t *states;
    uint32_t num_states;

    /* Interrupts: HI_INTR_ACKNOWLEDGE is read-to-clear. */
    uint32_t intr_pending;

    /* Front end (wait/link FE). */
    bool fe_running;
    uint32_t fe_addr;           /* address of the command being executed */
    uint32_t fe_cmd[2];         /* first two words of the current command */
    uint32_t fe_return_addr;    /* CALL/RETURN, one level */
    bool fe_call_active;
    bool fe_chip_selected;      /* CHIP_SELECT includes chip 0 */
    uint32_t pipe;
    int64_t wait_backoff_ns;

    /* MMUv2 exception state. */
    uint32_t mmu_status;
    uint32_t mmu_exception_addr[4];

    /* Statistics for the "draws not executed" warning. */
    uint64_t draws_3d_skipped;

    /* Properties: identification registers. */
    uint32_t chip_model;
    uint32_t chip_revision;
    uint32_t chip_date;
    uint32_t chip_time;
    uint32_t product_id;
    uint32_t eco_id;
    uint32_t customer_id;
    uint32_t features[8];       /* HI_CHIP_FEATURE, MINOR_FEATURE_0..6 */
    /* Properties: FE WAIT polling interval range. */
    uint32_t wait_poll_min_ns;
    uint32_t wait_poll_max_ns;
    uint32_t fe_batch;
};

/* Feature bits used by the model (HI_CHIP_FEATURE). */
#define VIV_FEATURE_PIPE_3D     BIT(2)
#define VIV_FEATURE_PIPE_2D     BIT(9)

static inline uint32_t viv_state(VivGpuState *s, uint32_t byte_addr)
{
    return s->states[(byte_addr >> 2) & (VIV_NUM_STATES - 1)];
}

/* GPU memory access through the MMU; faults are reported and redirected. */
bool viv_dma_read(VivGpuState *s, uint32_t va, void *buf, uint32_t len,
                  int unit);
bool viv_dma_write(VivGpuState *s, uint32_t va, const void *buf, uint32_t len,
                   int unit);

/* 2D engine (vivante_de.c). */
void viv_de_draw(VivGpuState *s, const uint32_t *rects, unsigned nrects,
                 const uint32_t *data, unsigned ndata);
void viv_de_vr_start(VivGpuState *s, uint32_t mode);

#endif
