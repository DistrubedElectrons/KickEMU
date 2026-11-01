/*
 * Vivante GPU core (GCNanoUltra / GC520L of the i.MX8M Mini)
 *
 * One instance models one Vivante core with its AHB register interface:
 * the host interface (HI) clock, idle, AXI and interrupt registers, the
 * identification and feature registers, the power management registers,
 * the MMUv2 and the wait/link front end (FE) that fetches and executes
 * command buffers. The i.MX8MM has two such cores (imx8mm.dtsi gpu@38000000):
 *
 *   iobase_3d 0x38000000, GIC SPI 3:  GC600 rev 0x4653 (GCNanoUltra), 3D pipe
 *   iobase_2d 0x38008000, GIC SPI 25: GC520 rev 0x5341 (GC520L), 2D pipe
 *
 * The register layout and command stream follow the etnaviv rnndb
 * (state_hi.xml, cmdstream.xml, state.xml, state_2d.xml); the behaviour the
 * driver relies on follows galcore 6.4.3.p1 (kernel-module-imx-gpu-viv,
 * hal/kernel/arch/gc_hal_kernel_hardware.c and _waitlink_fe.c,
 * gc_hal_kernel_hardware_func.c for the MMU setup command buffer).
 *
 * Execution model: command buffers run synchronously in a virtual-clock
 * timer callback. The FE keeps "running" while it spins in the WAIT/LINK
 * loop galcore leaves it in; the loop is re-fetched from guest memory with
 * an adaptive interval so that a WAIT patched into a LINK is followed.
 * 2D draws execute in vivante_de.c. 3D draws are parsed and skipped with a
 * LOG_UNIMP message, their events and fences still complete.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/kick/vivante_gpu.h"
#include "hw/kick/vivante-ids.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"

/* Host interface registers (state_hi.xml). */
#define HI_CLOCK_CONTROL        0x000
#define HI_IDLE_STATE           0x004
#define HI_AXI_CONFIG           0x008
#define HI_AXI_STATUS           0x00c
#define HI_INTR_ACKNOWLEDGE     0x010
#define HI_INTR_ENBL            0x014
#define HI_CHIP_IDENTITY        0x018
#define HI_CHIP_FEATURE         0x01c
#define HI_CHIP_MODEL           0x020
#define HI_CHIP_REV             0x024
#define HI_CHIP_DATE            0x028
#define HI_CHIP_TIME            0x02c
#define HI_CHIP_CUSTOMER_ID     0x030
#define HI_CHIP_MINOR_FEATURE_0 0x034
#define HI_CHIP_MINOR_FEATURE_1 0x074
#define HI_CHIP_MINOR_FEATURE_2 0x084
#define HI_CHIP_MINOR_FEATURE_3 0x088
#define HI_CHIP_MINOR_FEATURE_4 0x094
#define HI_CHIP_MINOR_FEATURE_5 0x0a0
#define HI_CHIP_INFO            0x0a4
#define HI_CHIP_PRODUCT_ID      0x0a8
#define HI_BLT_INTR             0x0d4
#define HI_CHIP_MINOR_FEATURE_6 0x0dc
#define HI_CHIP_ECO_ID          0x0e8
#define PM_POWER_CONTROLS       0x100
#define PM_MODULE_CONTROLS      0x104
#define PM_MODULE_STATUS        0x108
#define PM_PULSE_EATER          0x10c
#define MMUV2_SAFE_ADDRESS      0x180
#define MMUV2_CONFIGURATION     0x184
#define MMUV2_STATUS            0x188
#define MMUV2_CONTROL           0x18c
#define MMUV2_EXCEPTION_ADDR    0x190   /* four registers */
#define MMUV2_SEC_CONTROL       0x388   /* SECURITY_AHB cores only */
#define MMUV2_SEC_CMD_CTRL      0x3a4
#define MMUV2_SEC_AHB_CONTROL   0x3a8
#define FE_COMMAND_ADDRESS      0x654
#define FE_COMMAND_CONTROL      0x658
#define FE_DMA_STATUS           0x65c
#define FE_DMA_DEBUG_STATE      0x660
#define FE_DMA_ADDRESS          0x664
#define FE_DMA_LOW              0x668
#define FE_DMA_HIGH             0x66c

#define CLOCK_CONTROL_FSCALE_CMD_LOAD   BIT(9)
#define CLOCK_CONTROL_SOFT_RESET        BIT(12)
#define CLOCK_CONTROL_IDLE_3D           BIT(16)
#define CLOCK_CONTROL_IDLE_2D           BIT(17)
/* Value galcore writes before and after reset (gckHARDWARE_Construct). */
#define CLOCK_CONTROL_RESET             0x00010900

/* Every module idle; bit 31 (AXI low power) clear. */
#define IDLE_STATE_ALL                  0x7fffffff
#define IDLE_STATE_FE                   BIT(0)

#define INTR_MMU_EXCEPTION              BIT(30)
#define INTR_AXI_BUS_ERROR              BIT(31)

#define MMUV2_CONFIGURATION_MODE1K      BIT(0)
#define MMUV2_CONFIGURATION_MODE_MASK   BIT(3)
#define MMUV2_CONFIGURATION_FLUSH       BIT(4)
#define MMUV2_CONFIGURATION_FLUSH_MASK  BIT(7)
#define MMUV2_CONFIGURATION_ADDR_MASK   BIT(8)
#define MMUV2_CONFIGURATION_ADDRESS     0xfffffc00
#define MMUV2_CONTROL_ENABLE            BIT(0)

#define FE_COMMAND_CONTROL_ENABLE       BIT(16)
#define FE_COMMAND_CONTROL_PREFETCH     0xffff

/* FE opcodes (cmdstream.xml), bits 31:27 of the header. */
#define FE_OP_LOAD_STATE                0x01
#define FE_OP_END                       0x02
#define FE_OP_NOP                       0x03
#define FE_OP_DRAW_2D                   0x04
#define FE_OP_DRAW_PRIMITIVES           0x05
#define FE_OP_DRAW_INDEXED_PRIMITIVES   0x06
#define FE_OP_WAIT                      0x07
#define FE_OP_LINK                      0x08
#define FE_OP_STALL                     0x09
#define FE_OP_CALL                      0x0a
#define FE_OP_RETURN                    0x0b
#define FE_OP_DRAW_INSTANCED            0x0c
#define FE_OP_CHIP_SELECT               0x0d
#define FE_OP_WAIT_FENCE                0x0f

#define END_EVENT_ENABLE                BIT(8)

/* GL states with side effects (state.xml, byte addresses). */
#define GL_PIPE_SELECT                  0x3800
#define GL_EVENT                        0x3804
#define GL_SEMAPHORE_TOKEN              0x3808
#define GL_FLUSH_CACHE                  0x380c
#define GL_FLUSH_MMU                    0x3810
#define GL_MMU_FLUSH_EX                 0x3848  /* galcore MMU flush, 0x0e12 */

/* Start of the 2D engine VR (filter blit) trigger register. */
#define DE_VR_CONFIG                    0x1294

/* ------------------------------------------------------------------------ */
/* Interrupts                                                               */
/* ------------------------------------------------------------------------ */

static void viv_update_irq(VivGpuState *s)
{
    qemu_set_irq(s->irq, !!(s->intr_pending & s->regs[HI_INTR_ENBL / 4]));
}

static void viv_raise(VivGpuState *s, uint32_t bits)
{
    s->intr_pending |= bits;
    viv_update_irq(s);
}

/* ------------------------------------------------------------------------ */
/* MMUv2                                                                    */
/* ------------------------------------------------------------------------ */

static void viv_mmu_fault(VivGpuState *s, uint32_t va, int code, int unit)
{
    uint32_t shift = unit * 4;

    if (!((s->mmu_status >> shift) & 0xf)) {
        s->mmu_status |= code << shift;
        s->mmu_exception_addr[unit] = va;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: MMU exception %d at 0x%08x "
                  "(unit %d)\n", code, va, unit);
    viv_raise(s, INTR_MMU_EXCEPTION);
}

/*
 * Translate a GPU virtual address. Page table format (galcore
 * gc_hal_kernel_mmu.c, etnaviv_iommu_v2.c):
 *  - CONFIGURATION.MODE 1: 1 KiB master TLB, 256 entries of 16 MiB,
 *    MODE 0: 4 KiB master TLB, 1024 entries of 4 MiB;
 *  - MTLB entry: bit 0 present, bit 1 exception, bits 3:2 page size
 *    (4K, 64K, 1M, 16M), bits 31:6 slave TLB address;
 *  - STLB entry: bit 0 present, bit 1 exception, bit 2 writable,
 *    bits 11:4 physical address bits 39:32, upper bits the page address.
 * Faulting accesses are redirected into the safe page, like the hardware.
 */
static bool viv_translate(VivGpuState *s, uint32_t va, bool is_write,
                          int unit, hwaddr *pa, uint32_t *page_left)
{
    uint32_t config = s->regs[MMUV2_CONFIGURATION / 4];
    unsigned mtlb_shift = (config & MMUV2_CONFIGURATION_MODE1K) ? 24 : 22;
    static const unsigned page_shift[4] = { 12, 16, 20, 24 };
    hwaddr mtlb = config & MMUV2_CONFIGURATION_ADDRESS;
    uint32_t mte, ste, idx;
    unsigned ps;
    int code;

    if (!(s->regs[MMUV2_CONTROL / 4] & MMUV2_CONTROL_ENABLE)) {
        *pa = va;
        *page_left = 0x1000 - (va & 0xfff);
        return true;
    }

    mte = address_space_ldl_le(&s->dma_as, mtlb + (va >> mtlb_shift) * 4,
                               MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(mte & 1)) {
        code = VIV_MMU_SLAVE_NOT_PRESENT;
        goto fault;
    }
    ps = page_shift[(mte >> 2) & 3];
    idx = (va & ((1u << mtlb_shift) - 1)) >> MIN(ps, mtlb_shift);
    ste = address_space_ldl_le(&s->dma_as, (mte & ~0x3fu) + idx * 4,
                               MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(ste & 1)) {
        code = VIV_MMU_PAGE_NOT_PRESENT;
        goto fault;
    }
    if (is_write && !(ste & BIT(2))) {
        code = VIV_MMU_WRITE_VIOLATION;
        goto fault;
    }
    *pa = ((hwaddr)((ste >> 4) & 0xff) << 32) |
          (ste & ~((1u << ps) - 1)) | (va & ((1u << ps) - 1));
    *page_left = (1u << ps) - (va & ((1u << ps) - 1));
    return true;

fault:
    viv_mmu_fault(s, va, code, unit);
    *pa = s->regs[MMUV2_SAFE_ADDRESS / 4] + (va & 0xfff);
    *page_left = 0x1000 - (va & 0xfff);
    return false;
}

static bool viv_dma(VivGpuState *s, uint32_t va, void *buf, uint32_t len,
                    bool is_write, int unit)
{
    uint8_t *p = buf;
    bool ok = true;

    while (len) {
        hwaddr pa;
        uint32_t chunk;

        ok &= viv_translate(s, va, is_write, unit, &pa, &chunk);
        chunk = MIN(chunk, len);
        if (dma_memory_rw(&s->dma_as, pa, p, chunk,
                          is_write ? DMA_DIRECTION_FROM_DEVICE :
                                     DMA_DIRECTION_TO_DEVICE,
                          MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: AXI error at 0x%"
                          HWADDR_PRIx "\n", pa);
            s->regs[HI_AXI_STATUS / 4] |= is_write ? BIT(8) : BIT(9);
            viv_raise(s, INTR_AXI_BUS_ERROR);
            ok = false;
        }
        va += chunk;
        p += chunk;
        len -= chunk;
    }
    return ok;
}

bool viv_dma_read(VivGpuState *s, uint32_t va, void *buf, uint32_t len,
                  int unit)
{
    return viv_dma(s, va, buf, len, false, unit);
}

bool viv_dma_write(VivGpuState *s, uint32_t va, const void *buf, uint32_t len,
                   int unit)
{
    return viv_dma(s, va, (void *)buf, len, true, unit);
}

/* MMUv2 CONFIGURATION is a masked register: a set _MASK bit keeps a field. */
static void viv_mmu_write_config(VivGpuState *s, uint32_t v)
{
    uint32_t cur = s->regs[MMUV2_CONFIGURATION / 4];

    if (!(v & MMUV2_CONFIGURATION_MODE_MASK)) {
        cur = (cur & ~MMUV2_CONFIGURATION_MODE1K) |
              (v & MMUV2_CONFIGURATION_MODE1K);
    }
    if (!(v & MMUV2_CONFIGURATION_ADDR_MASK)) {
        cur = (cur & ~MMUV2_CONFIGURATION_ADDRESS) |
              (v & MMUV2_CONFIGURATION_ADDRESS);
    }
    /* FLUSH: the model keeps no TLB, every access walks the tables. */
    s->regs[MMUV2_CONFIGURATION / 4] = cur;
}

/* ------------------------------------------------------------------------ */
/* State loading                                                            */
/* ------------------------------------------------------------------------ */

/*
 * Masked 2D states (state_2d.xml masked="yes"): each field is only written
 * when its _MASK bit in the written value is zero.
 */
typedef struct VivMaskedField {
    uint32_t field;
    uint32_t mask_bit;
} VivMaskedField;

typedef struct VivMaskedState {
    uint32_t addr;
    VivMaskedField f[4];
} VivMaskedState;

static const VivMaskedState viv_masked_states[] = {
    { 0x1294, { { 0x00000003, BIT(3) } } },                  /* VR_CONFIG */
    { 0x12b0, { { 0x00000003, BIT(3) } } },                  /* PE_CONFIG */
    { 0x12bc, { { 0x00000007, BIT(8) }, { 0x00000038, BIT(9) },
                { 0x00003000, BIT(15) }, { 0x00030000, BIT(19) } } },
                                                             /* ROT_ANGLE */
    { 0x12d4, { { 0x00000333, BIT(12) }, { 0x03330000, BIT(28) },
                { BIT(29), BIT(31) } } },                    /* PE_TRANSPARENCY */
    { 0x12d8, { { BIT(0), BIT(3) }, { BIT(4), BIT(7) },
                { BIT(8), BIT(11) } } },                     /* PE_CONTROL */
    { 0x12e4, { { 0x00000003, BIT(3) }, { 0x000000f0, BIT(8) } } },
                                                             /* VR_CONFIG_EX */
    { 0x12f0, { { BIT(0), BIT(3) }, { BIT(4), BIT(7) }, { BIT(8), BIT(11) },
                { BIT(12), BIT(15) } } },                    /* BW_CONFIG */
};

static bool viv_masked_write(VivGpuState *s, uint32_t addr, uint32_t value)
{
    unsigned i, j;

    for (i = 0; i < ARRAY_SIZE(viv_masked_states); i++) {
        const VivMaskedState *m = &viv_masked_states[i];
        uint32_t cur;

        if (m->addr != addr) {
            continue;
        }
        cur = s->states[addr >> 2];
        for (j = 0; j < ARRAY_SIZE(m->f) && m->f[j].field; j++) {
            if (!(value & m->f[j].mask_bit)) {
                cur = (cur & ~m->f[j].field) | (value & m->f[j].field);
            }
        }
        s->states[addr >> 2] = cur;
        return true;
    }
    return false;
}

static bool viv_has_pipe(VivGpuState *s, uint32_t pipe)
{
    return s->features[0] & (pipe == VIV_PIPE_2D ? VIV_FEATURE_PIPE_2D :
                                                   VIV_FEATURE_PIPE_3D);
}

static void viv_load_state(VivGpuState *s, uint32_t index, uint32_t value)
{
    uint32_t addr = index << 2;

    /* The first 4 KiB of state space alias the AHB registers. */
    if (addr < 0x1000) {
        switch (addr) {
        case MMUV2_SAFE_ADDRESS:
            s->regs[addr / 4] = value;
            return;
        case MMUV2_CONFIGURATION:
            viv_mmu_write_config(s, value);
            return;
        default:
            qemu_log_mask(LOG_UNIMP, "vivante-gpu: LOAD_STATE to register "
                          "0x%03x not modelled\n", addr);
            return;
        }
    }

    if (!viv_masked_write(s, addr, value)) {
        s->states[index] = value;
    }

    switch (addr) {
    case GL_PIPE_SELECT:
        if (!viv_has_pipe(s, value & 1)) {
            qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: select of pipe %u "
                          "this core does not have\n", value & 1);
        }
        s->pipe = value & 1;
        break;
    case GL_EVENT:
        /*
         * Events from the FE, PE or BLT fire once all earlier work in that
         * unit is done, which is always the case with synchronous draws.
         */
        viv_raise(s, BIT(value & 0x1f));
        break;
    case GL_SEMAPHORE_TOKEN:
    case GL_FLUSH_CACHE:
    case GL_FLUSH_MMU:
    case GL_MMU_FLUSH_EX:
        /* Caches and TLBs are not modelled; nothing is outstanding. */
        break;
    case DE_VR_CONFIG:
        /* Masked register: START (1:0) is applied when START_MASK is 0. */
        if (s->pipe == VIV_PIPE_2D && !(value & BIT(3))) {
            viv_de_vr_start(s, value & 3);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Front end                                                                */
/* ------------------------------------------------------------------------ */

static void viv_fe_stop(VivGpuState *s, uint32_t next)
{
    s->fe_running = false;
    s->fe_addr = next;
    timer_del(s->fe_timer);
}

static void viv_fe_kick(VivGpuState *s, int64_t delay_ns)
{
    timer_mod(s->fe_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void viv_fe_error(VivGpuState *s, const char *why)
{
    qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: FE stopped at 0x%08x: %s "
                  "(0x%08x 0x%08x)\n", s->fe_addr, why, s->fe_cmd[0],
                  s->fe_cmd[1]);
    s->fe_running = false;
    timer_del(s->fe_timer);
}

static void viv_skip_3d_draw(VivGpuState *s, const char *what)
{
    s->draws_3d_skipped++;
    warn_report_once("vivante-gpu: 3D draws are not executed by this model, "
                     "rendering results will be missing");
    qemu_log_mask(LOG_UNIMP, "vivante-gpu: %s at 0x%08x skipped (3D pipe not "
                  "implemented, %" PRIu64 " so far)\n", what, s->fe_addr,
                  s->draws_3d_skipped);
}

/*
 * Execute commands until the FE stops, reaches a WAIT, or the batch limit
 * is hit. Returns the delay before the next run, or -1 if the FE stopped.
 */
static int64_t viv_fe_run(VivGpuState *s)
{
    unsigned budget = s->fe_batch;
    bool progress = false;

    while (s->fe_running && budget--) {
        uint32_t hdr, op, words;
        uint32_t addr = s->fe_addr;

        if (!viv_dma_read(s, addr, s->fe_cmd, 8, VIV_MMU_UNIT_FE)) {
            viv_fe_error(s, "command fetch fault");
            return -1;
        }
        hdr = s->fe_cmd[0];
        op = hdr >> 27;

        /*
         * While CHIP_SELECT excludes chip 0, state loads, draws and events
         * are decoded for their size but not executed; flow control
         * (LINK, CALL, RETURN, WAIT, END) always runs.
         */
        switch (op) {
        case FE_OP_LOAD_STATE: {
            uint32_t count = (hdr >> 16) & 0x3ff;
            uint32_t index = hdr & 0xffff;
            g_autofree uint32_t *vals = NULL;
            uint32_t i;

            if (!count) {
                count = 0x400;
            }
            words = ROUND_UP(1 + count, 2);
            vals = g_new(uint32_t, count);
            if (!viv_dma_read(s, addr + 4, vals, count * 4, VIV_MMU_UNIT_FE)) {
                viv_fe_error(s, "LOAD_STATE fetch fault");
                return -1;
            }
            if (s->fe_chip_selected) {
                for (i = 0; i < count; i++) {
                    viv_load_state(s, (index + i) & 0xffff, vals[i]);
                }
            }
            progress = true;
            break;
        }
        case FE_OP_END:
            if (s->fe_chip_selected && (hdr & END_EVENT_ENABLE)) {
                viv_raise(s, BIT(hdr & 0x1f));
            }
            /* The DMA address register then points past the END. */
            viv_fe_stop(s, addr + 8);
            return -1;
        case FE_OP_NOP:
            words = 2;
            break;
        case FE_OP_DRAW_2D: {
            uint32_t nrects = (hdr >> 8) & 0xff;
            uint32_t ndata = (hdr >> 16) & 0x7ff;
            g_autofree uint32_t *buf = NULL;

            words = ROUND_UP(2 + nrects * 2 + ndata, 2);
            buf = g_new(uint32_t, words);
            if (!viv_dma_read(s, addr, buf, words * 4, VIV_MMU_UNIT_FE)) {
                viv_fe_error(s, "DRAW_2D fetch fault");
                return -1;
            }
            if (!s->fe_chip_selected) {
                break;
            }
            if (s->pipe != VIV_PIPE_2D || !viv_has_pipe(s, VIV_PIPE_2D)) {
                qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: DRAW_2D without "
                              "the 2D pipe selected or present\n");
            } else {
                viv_de_draw(s, buf + 2, nrects, buf + 2 + nrects * 2, ndata);
            }
            progress = true;
            break;
        }
        case FE_OP_DRAW_PRIMITIVES:
            words = 4;
            if (s->fe_chip_selected) {
                viv_skip_3d_draw(s, "DRAW_PRIMITIVES");
            }
            progress = true;
            break;
        case FE_OP_DRAW_INDEXED_PRIMITIVES:
            words = 6;
            if (s->fe_chip_selected) {
                viv_skip_3d_draw(s, "DRAW_INDEXED_PRIMITIVES");
            }
            progress = true;
            break;
        case FE_OP_DRAW_INSTANCED:
            words = 4;
            if (s->fe_chip_selected) {
                viv_skip_3d_draw(s, "DRAW_INSTANCED");
            }
            progress = true;
            break;
        case FE_OP_WAIT:
            /*
             * Pause, then fetch the next command (normally the LINK back to
             * this WAIT). Back off while nothing new arrives.
             */
            s->fe_addr = addr + 8;
            if (progress) {
                s->wait_backoff_ns = s->wait_poll_min_ns;
            } else {
                s->wait_backoff_ns = MIN(s->wait_backoff_ns * 2,
                                         (int64_t)s->wait_poll_max_ns);
            }
            /* Report the WAIT itself as the current DMA position. */
            s->fe_addr = addr;
            return s->wait_backoff_ns;
        case FE_OP_LINK: {
            uint32_t target = s->fe_cmd[1];

            if ((target & 7) != 0) {
                viv_fe_error(s, "LINK to unaligned address");
                return -1;
            }
            s->fe_addr = target;
            /* A LINK to the WAIT just left is the idle loop, not progress. */
            continue;
        }
        case FE_OP_STALL:
            /* Every unit is idle when the FE reaches the stall. */
            words = 2;
            break;
        case FE_OP_CALL: {
            uint32_t buf[4];

            if (!viv_dma_read(s, addr, buf, 16, VIV_MMU_UNIT_FE)) {
                viv_fe_error(s, "CALL fetch fault");
                return -1;
            }
            if (s->fe_call_active) {
                viv_fe_error(s, "nested CALL");
                return -1;
            }
            s->fe_call_active = true;
            s->fe_return_addr = buf[3];
            s->fe_addr = buf[1];
            progress = true;
            continue;
        }
        case FE_OP_RETURN:
            if (!s->fe_call_active) {
                viv_fe_error(s, "RETURN without CALL");
                return -1;
            }
            s->fe_call_active = false;
            s->fe_addr = s->fe_return_addr;
            continue;
        case FE_OP_CHIP_SELECT:
            /* Single-chip configuration: this is chip 0. */
            s->fe_chip_selected = hdr & 1;
            words = 2;
            break;
        default:
            viv_fe_error(s, "unknown or unsupported FE opcode");
            return -1;
        }
        s->fe_addr = addr + words * 4;
    }

    if (!s->fe_running) {
        return -1;
    }
    /* Batch exhausted: continue soon without starving the main loop. */
    return 0;
}

static void viv_fe_timer(void *opaque)
{
    VivGpuState *s = opaque;
    int64_t delay;

    if (!s->fe_running) {
        return;
    }
    /* Resume after a WAIT at the command following it. */
    if ((s->fe_cmd[0] >> 27) == FE_OP_WAIT) {
        s->fe_addr += 8;
        s->fe_cmd[0] = 0;
    }
    delay = viv_fe_run(s);
    if (delay >= 0) {
        viv_fe_kick(s, delay);
    }
}

static void viv_fe_start(VivGpuState *s)
{
    s->fe_addr = s->regs[FE_COMMAND_ADDRESS / 4];
    s->fe_cmd[0] = s->fe_cmd[1] = 0;
    s->fe_running = true;
    s->fe_call_active = false;
    s->wait_backoff_ns = s->wait_poll_min_ns;
    viv_fe_kick(s, 0);
}

/* ------------------------------------------------------------------------ */
/* Registers                                                                */
/* ------------------------------------------------------------------------ */

static void viv_soft_reset(VivGpuState *s)
{
    uint32_t clock = s->regs[HI_CLOCK_CONTROL / 4];

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->states, 0, VIV_NUM_STATES * sizeof(uint32_t));
    s->regs[HI_CLOCK_CONTROL / 4] = clock;
    s->intr_pending = 0;
    s->fe_running = false;
    s->fe_addr = 0;
    s->fe_cmd[0] = s->fe_cmd[1] = 0;
    s->fe_call_active = false;
    s->fe_chip_selected = true;
    s->pipe = viv_has_pipe(s, VIV_PIPE_3D) ? VIV_PIPE_3D : VIV_PIPE_2D;
    s->mmu_status = 0;
    memset(s->mmu_exception_addr, 0, sizeof(s->mmu_exception_addr));
    if (s->fe_timer) {
        timer_del(s->fe_timer);
    }
    viv_update_irq(s);
}

static uint32_t viv_idle_state(VivGpuState *s)
{
    return s->fe_running ? IDLE_STATE_ALL & ~IDLE_STATE_FE : IDLE_STATE_ALL;
}

static uint64_t viv_read(void *opaque, hwaddr off, unsigned size)
{
    VivGpuState *s = opaque;
    uint32_t v;

    if (off >= 0x1000) {
        qemu_log_mask(LOG_UNIMP, "vivante-gpu: read of 0x%" HWADDR_PRIx
                      " outside the modelled register file\n", off);
        return 0;
    }

    switch (off) {
    case HI_CLOCK_CONTROL:
        /* The 3D and 2D pipes are idle whenever the FE is between draws. */
        return s->regs[off / 4] | CLOCK_CONTROL_IDLE_3D | CLOCK_CONTROL_IDLE_2D;
    case HI_IDLE_STATE:
        return viv_idle_state(s);
    case HI_INTR_ACKNOWLEDGE:
        v = s->intr_pending;
        s->intr_pending = 0;
        viv_update_irq(s);
        return v;
    case HI_CHIP_IDENTITY:
        /* Pre-GC500 identity format; zero high byte means "use CHIP_MODEL". */
        return 0;
    case HI_CHIP_FEATURE:
        return s->features[0];
    case HI_CHIP_MODEL:
        return s->chip_model;
    case HI_CHIP_REV:
        return s->chip_revision;
    case HI_CHIP_DATE:
        return s->chip_date;
    case HI_CHIP_TIME:
        return s->chip_time;
    case HI_CHIP_CUSTOMER_ID:
        return s->customer_id;
    case HI_CHIP_MINOR_FEATURE_0:
        return s->features[1];
    case HI_CHIP_MINOR_FEATURE_1:
        return s->features[2];
    case HI_CHIP_MINOR_FEATURE_2:
        return s->features[3];
    case HI_CHIP_MINOR_FEATURE_3:
        return s->features[4];
    case HI_CHIP_MINOR_FEATURE_4:
        return s->features[5];
    case HI_CHIP_MINOR_FEATURE_5:
        return s->features[6];
    case HI_CHIP_MINOR_FEATURE_6:
        return s->features[7];
    case HI_CHIP_PRODUCT_ID:
        return s->product_id;
    case HI_CHIP_ECO_ID:
        return s->eco_id;
    case HI_BLT_INTR:
        /* No asynchronous BLT front end on these cores. */
        return 0;
    case PM_MODULE_STATUS:
        /* No module is clock gated while the model runs. */
        return 0;
    case MMUV2_STATUS:
        /* Read to clear, like the interrupt acknowledge it accompanies. */
        v = s->mmu_status;
        s->mmu_status = 0;
        return v;
    case MMUV2_EXCEPTION_ADDR ... MMUV2_EXCEPTION_ADDR + 0xc:
        return s->mmu_exception_addr[(off - MMUV2_EXCEPTION_ADDR) / 4];
    case MMUV2_SEC_CONTROL:
    case MMUV2_SEC_CMD_CTRL:
    case MMUV2_SEC_AHB_CONTROL:
        qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: secure MMU register 0x%"
                      HWADDR_PRIx " on a core without SECURITY_AHB\n", off);
        return 0;
    case FE_DMA_ADDRESS:
        return s->fe_addr;
    case FE_DMA_LOW:
        return s->fe_cmd[0];
    case FE_DMA_HIGH:
        return s->fe_cmd[1];
    case FE_DMA_STATUS:
    case FE_DMA_DEBUG_STATE:
        /* Debug state machines: report the FE at rest. */
        return 0;
    default:
        return s->regs[off / 4];
    }
}

static void viv_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VivGpuState *s = opaque;
    uint32_t v = value;

    if (off >= 0x1000) {
        qemu_log_mask(LOG_UNIMP, "vivante-gpu: write 0x%08x to 0x%" HWADDR_PRIx
                      " outside the modelled register file\n", v, off);
        return;
    }

    switch (off) {
    case HI_CLOCK_CONTROL:
        if (v & CLOCK_CONTROL_SOFT_RESET) {
            s->regs[off / 4] = v & ~CLOCK_CONTROL_SOFT_RESET;
            viv_soft_reset(s);
            return;
        }
        /* FSCALE_CMD_LOAD latches the frequency scale and self clears. */
        s->regs[off / 4] = v & ~(CLOCK_CONTROL_FSCALE_CMD_LOAD |
                                 CLOCK_CONTROL_IDLE_3D | CLOCK_CONTROL_IDLE_2D);
        break;
    case HI_IDLE_STATE:
    case HI_AXI_STATUS:
    case HI_CHIP_IDENTITY ... HI_CHIP_MINOR_FEATURE_0:
    case HI_CHIP_MINOR_FEATURE_1:
    case HI_CHIP_MINOR_FEATURE_2:
    case HI_CHIP_MINOR_FEATURE_3:
    case HI_CHIP_MINOR_FEATURE_4:
    case HI_CHIP_MINOR_FEATURE_5:
    case HI_CHIP_INFO:
    case HI_CHIP_PRODUCT_ID:
    case HI_CHIP_MINOR_FEATURE_6:
    case HI_CHIP_ECO_ID:
    case PM_MODULE_STATUS:
    case MMUV2_STATUS:
    case MMUV2_EXCEPTION_ADDR ... MMUV2_EXCEPTION_ADDR + 0xc:
    case FE_DMA_STATUS ... FE_DMA_HIGH:
        qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: write 0x%08x to "
                      "read-only register 0x%" HWADDR_PRIx "\n", v, off);
        break;
    case HI_INTR_ACKNOWLEDGE:
        qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: write to read-to-clear "
                      "interrupt acknowledge\n");
        break;
    case HI_INTR_ENBL:
        s->regs[off / 4] = v;
        viv_update_irq(s);
        break;
    case MMUV2_CONFIGURATION:
        viv_mmu_write_config(s, v);
        break;
    case MMUV2_SEC_CONTROL:
    case MMUV2_SEC_CMD_CTRL:
    case MMUV2_SEC_AHB_CONTROL:
        qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: secure MMU register 0x%"
                      HWADDR_PRIx " on a core without SECURITY_AHB\n", off);
        break;
    case FE_COMMAND_CONTROL:
        s->regs[off / 4] = v;
        if (v & FE_COMMAND_CONTROL_ENABLE) {
            if (!(v & FE_COMMAND_CONTROL_PREFETCH)) {
                qemu_log_mask(LOG_GUEST_ERROR, "vivante-gpu: FE started with "
                              "zero prefetch\n");
            }
            viv_fe_start(s);
        }
        break;
    default:
        /*
         * Configuration and debug registers without side effects in the
         * model (AXI, PM controls, pulse eater, MC/MMUv1 base addresses,
         * debug selects): kept so they read back.
         */
        s->regs[off / 4] = v;
        break;
    }
}

static const MemoryRegionOps viv_ops = {
    .read = viv_read,
    .write = viv_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------------ */

static void viv_reset_hold(Object *obj, ResetType type)
{
    VivGpuState *s = VIVANTE_GPU(obj);

    s->regs[HI_CLOCK_CONTROL / 4] = CLOCK_CONTROL_RESET;
    viv_soft_reset(s);
    s->draws_3d_skipped = 0;
}

static void viv_init(Object *obj)
{
    VivGpuState *s = VIVANTE_GPU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &viv_ops, s, TYPE_VIVANTE_GPU,
                          VIV_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->num_states = VIV_NUM_STATES;
    s->states = g_new0(uint32_t, VIV_NUM_STATES);
}

static void viv_finalize(Object *obj)
{
    VivGpuState *s = VIVANTE_GPU(obj);

    g_free(s->states);
}

static void viv_realize(DeviceState *dev, Error **errp)
{
    VivGpuState *s = VIVANTE_GPU(dev);

    if (!s->chip_model) {
        error_setg(errp, "chip-model must be set");
        return;
    }
    if (!s->wait_poll_min_ns || s->wait_poll_max_ns < s->wait_poll_min_ns) {
        error_setg(errp, "invalid wait-poll-min-ns/wait-poll-max-ns");
        return;
    }
    if (!s->fe_batch) {
        error_setg(errp, "fe-batch must be non-zero");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr ? s->dma_mr : get_system_memory(),
                       "vivante-gpu-dma");
    s->fe_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, viv_fe_timer, s);
}

static const Property viv_properties[] = {
    DEFINE_PROP_UINT32("chip-model", VivGpuState, chip_model, 0),
    DEFINE_PROP_UINT32("chip-revision", VivGpuState, chip_revision, 0),
    DEFINE_PROP_UINT32("chip-date", VivGpuState, chip_date, 0),
    DEFINE_PROP_UINT32("chip-time", VivGpuState, chip_time, 0),
    DEFINE_PROP_UINT32("product-id", VivGpuState, product_id, 0),
    DEFINE_PROP_UINT32("eco-id", VivGpuState, eco_id, 0),
    DEFINE_PROP_UINT32("customer-id", VivGpuState, customer_id, 0),
    DEFINE_PROP_UINT32("chip-features", VivGpuState, features[0], 0),
    DEFINE_PROP_UINT32("minor-features0", VivGpuState, features[1], 0),
    DEFINE_PROP_UINT32("minor-features1", VivGpuState, features[2], 0),
    DEFINE_PROP_UINT32("minor-features2", VivGpuState, features[3], 0),
    DEFINE_PROP_UINT32("minor-features3", VivGpuState, features[4], 0),
    DEFINE_PROP_UINT32("minor-features4", VivGpuState, features[5], 0),
    DEFINE_PROP_UINT32("minor-features5", VivGpuState, features[6], 0),
    DEFINE_PROP_UINT32("minor-features6", VivGpuState, features[7], 0),
    /* WAIT/LINK re-fetch interval, doubled while the FE idles. */
    DEFINE_PROP_UINT32("wait-poll-min-ns", VivGpuState, wait_poll_min_ns,
                       10000),
    DEFINE_PROP_UINT32("wait-poll-max-ns", VivGpuState, wait_poll_max_ns,
                       1000000),
    /* Commands executed per timer callback before yielding. */
    DEFINE_PROP_UINT32("fe-batch", VivGpuState, fe_batch, 4096),
    DEFINE_PROP_LINK("dma-mr", VivGpuState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static const VMStateDescription vmstate_viv = {
    .name = TYPE_VIVANTE_GPU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, VivGpuState, VIV_NUM_REGS),
        VMSTATE_VARRAY_UINT32_ALLOC(states, VivGpuState, num_states, 1,
                                    vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(intr_pending, VivGpuState),
        VMSTATE_BOOL(fe_running, VivGpuState),
        VMSTATE_UINT32(fe_addr, VivGpuState),
        VMSTATE_UINT32_ARRAY(fe_cmd, VivGpuState, 2),
        VMSTATE_UINT32(fe_return_addr, VivGpuState),
        VMSTATE_BOOL(fe_call_active, VivGpuState),
        VMSTATE_BOOL(fe_chip_selected, VivGpuState),
        VMSTATE_UINT32(pipe, VivGpuState),
        VMSTATE_INT64(wait_backoff_ns, VivGpuState),
        VMSTATE_UINT32(mmu_status, VivGpuState),
        VMSTATE_UINT32_ARRAY(mmu_exception_addr, VivGpuState, 4),
        VMSTATE_TIMER_PTR(fe_timer, VivGpuState),
        VMSTATE_END_OF_LIST()
    },
};

static void viv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Vivante GPU core";
    dc->realize = viv_realize;
    dc->vmsd = &vmstate_viv;
    device_class_set_props(dc, viv_properties);
    rc->phases.hold = viv_reset_hold;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/*
 * i.MX8MM cores. The identification comes from the galcore feature database
 * rows for these chips (vivante-ids.h); CHIP_DATE/CHIP_TIME are not in the
 * database and stay zero unless set.
 */
static void imx8mm_gpu3d_init(Object *obj)
{
    VivGpuState *s = VIVANTE_GPU(obj);

    s->chip_model = VIV_GC600_4653_MODEL;
    s->chip_revision = VIV_GC600_4653_REVISION;
    s->product_id = VIV_GC600_4653_PRODUCT_ID;
    s->eco_id = VIV_GC600_4653_ECO_ID;
    s->customer_id = VIV_GC600_4653_CUSTOMER_ID;
    s->features[0] = VIV_GC600_4653_CHIP_FEATURES;
    s->features[1] = VIV_GC600_4653_MINOR_FEATURES0;
    s->features[2] = VIV_GC600_4653_MINOR_FEATURES1;
    s->features[3] = VIV_GC600_4653_MINOR_FEATURES2;
    s->features[4] = VIV_GC600_4653_MINOR_FEATURES3;
    s->features[5] = VIV_GC600_4653_MINOR_FEATURES4;
    s->features[6] = VIV_GC600_4653_MINOR_FEATURES5;
    s->features[7] = VIV_GC600_4653_MINOR_FEATURES6;
}

static void imx8mm_gpu2d_init(Object *obj)
{
    VivGpuState *s = VIVANTE_GPU(obj);

    s->chip_model = VIV_GC520_5341_MODEL;
    s->chip_revision = VIV_GC520_5341_REVISION;
    s->product_id = VIV_GC520_5341_PRODUCT_ID;
    s->eco_id = VIV_GC520_5341_ECO_ID;
    s->customer_id = VIV_GC520_5341_CUSTOMER_ID;
    s->features[0] = VIV_GC520_5341_CHIP_FEATURES;
    s->features[1] = VIV_GC520_5341_MINOR_FEATURES0;
    s->features[2] = VIV_GC520_5341_MINOR_FEATURES1;
    s->features[3] = VIV_GC520_5341_MINOR_FEATURES2;
    s->features[4] = VIV_GC520_5341_MINOR_FEATURES3;
    s->features[5] = VIV_GC520_5341_MINOR_FEATURES4;
    s->features[6] = VIV_GC520_5341_MINOR_FEATURES5;
    s->features[7] = VIV_GC520_5341_MINOR_FEATURES6;
}

static const TypeInfo viv_types[] = {
    {
        .name = TYPE_VIVANTE_GPU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(VivGpuState),
        .instance_init = viv_init,
        .instance_finalize = viv_finalize,
        .class_init = viv_class_init,
    },
    {
        .name = TYPE_IMX8MM_GPU3D,
        .parent = TYPE_VIVANTE_GPU,
        .instance_init = imx8mm_gpu3d_init,
    },
    {
        .name = TYPE_IMX8MM_GPU2D,
        .parent = TYPE_VIVANTE_GPU,
        .instance_init = imx8mm_gpu2d_init,
    },
};

DEFINE_TYPES(viv_types)
