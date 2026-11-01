/*
 * i.MX 8M Mini General Power Controller (GPC)
 *
 * IMX8MMRM chapter 5.2 "General Power Controller (GPC)". The GPC sequences
 * power for the A53 cores (CPU PGCs) and the peripheral power domains
 * (MIPI, PCIe, OTG1/2, HSIOMIX, DDRMIX, GPU2D/3D, GPUMIX, VPUMIX, DISPMIX,
 * VPU G1/G2/H1). Software requests a transition by setting a bit in
 * CPU_PGC_UP_TRG/CPU_PGC_DN_TRG or PU_PGC_UP_TRG/PU_PGC_DN_TRG and polls
 * until the bit self-clears; bus isolation is handled through the ADB400
 * handshake in GPC_PU_PWRHSK, where every SYNC request bit is acknowledged
 * on the ACK bit 18 positions higher.
 *
 * Power sequencing completes immediately and deterministically. The power
 * state of each domain is reflected in its PGC status register (PGSR,
 * +0xC in each 0x40 PGC block: bit 0 = powered down). The power-down of a
 * domain does not reset or isolate the blocks inside it (documented as a
 * known gap in docs/soc.md).
 *
 * The register users are ATF (plat/imx/imx8m/imx8mm/gpc.c and
 * gpc_common.c), U-Boot and the upstream Linux gpcv2 driver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#include "imx8mm_gpc.h"

#define GPC_PGC_CPU_0_1_MAPPING 0x0ec
#define GPC_CPU_PGC_UP_TRG      0x0f0
#define GPC_PU_PGC_UP_TRG       0x0f8
#define GPC_CPU_PGC_DN_TRG      0x0fc
#define GPC_PU_PGC_DN_TRG       0x104
#define GPC_PU_PWRHSK           0x1fc

/* PGC blocks: A53 cores at 0x800 + n*0x40, PLAT 0x900, NOC 0xa40, PU 0xc00.. */
#define GPC_PGC_BASE            0x800
#define GPC_PGC_END             0xf7f
#define GPC_PGC_SR              0xc     /* status register inside a block */
#define GPC_PGC_PU_BASE         0xc00

/* ADB400 handshake: SYNC request bits, each acknowledged at bit + 18 */
#define PWRHSK_SYNC_MASK        0x00000fe4
#define PWRHSK_ACK_SHIFT        18

#define R(off)                  ((off) / 4)

/* PU domain request bit n maps to PGC block GPC_PGC_PU_BASE + n * 0x40 */
#define GPC_NUM_PU_DOMAINS      14

static void imx8mm_gpc_set_pgc_state(IMX8MMGPCState *s, hwaddr pgc, bool down)
{
    uint32_t *sr = &s->regs[R(pgc + GPC_PGC_SR)];

    *sr = deposit32(*sr, 0, 1, down);
}

static void imx8mm_gpc_pu_trigger(IMX8MMGPCState *s, uint32_t req, bool down)
{
    for (int n = 0; n < GPC_NUM_PU_DOMAINS; n++) {
        hwaddr pgc = GPC_PGC_PU_BASE + n * 0x40;

        /* A domain only switches when its PGC control bit (PCR) is set */
        if ((req & BIT(n)) && (s->regs[R(pgc)] & 1)) {
            imx8mm_gpc_set_pgc_state(s, pgc, down);
        }
    }
}

static void imx8mm_gpc_cpu_trigger(IMX8MMGPCState *s, uint32_t req, bool down)
{
    for (int n = 0; n < 4; n++) {
        hwaddr pgc = GPC_PGC_BASE + n * 0x40;

        if (req & BIT(n)) {
            imx8mm_gpc_set_pgc_state(s, pgc, down);
        }
    }
}

static uint64_t imx8mm_gpc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMGPCState *s = IMX8MM_GPC(opaque);

    if (offset >= IMX8MM_GPC_NUM_REGS * 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_GPC, offset);
        return 0;
    }
    return s->regs[R(offset)];
}

static void imx8mm_gpc_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX8MMGPCState *s = IMX8MM_GPC(opaque);

    if (offset >= IMX8MM_GPC_NUM_REGS * 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_GPC, offset);
        return;
    }

    switch (offset) {
    case GPC_CPU_PGC_UP_TRG:
    case GPC_CPU_PGC_DN_TRG:
        /* Transition done at once: the request bits read back as 0 */
        imx8mm_gpc_cpu_trigger(s, value, offset == GPC_CPU_PGC_DN_TRG);
        s->regs[R(offset)] = 0;
        break;
    case GPC_PU_PGC_UP_TRG:
    case GPC_PU_PGC_DN_TRG:
        imx8mm_gpc_pu_trigger(s, value, offset == GPC_PU_PGC_DN_TRG);
        s->regs[R(offset)] = 0;
        break;
    case GPC_PU_PWRHSK:
        /* ACK bits are read-only and follow the SYNC requests */
        value &= ~(PWRHSK_SYNC_MASK << PWRHSK_ACK_SHIFT);
        value |= (value & PWRHSK_SYNC_MASK) << PWRHSK_ACK_SHIFT;
        s->regs[R(offset)] = value;
        break;
    default:
        if (offset >= GPC_PGC_BASE && offset <= GPC_PGC_END &&
            (offset & 0x3f) == GPC_PGC_SR) {
            /* PGSR is status, read-only */
            break;
        }
        s->regs[R(offset)] = value;
        break;
    }
}

static const MemoryRegionOps imx8mm_gpc_ops = {
    .read = imx8mm_gpc_read,
    .write = imx8mm_gpc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_gpc_reset(DeviceState *dev)
{
    IMX8MMGPCState *s = IMX8MM_GPC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * At POR the boot core and the always-on platform are powered, every
     * PU domain is off until software powers it up. Cores 1..3 start
     * powered down.
     */
    for (int n = 1; n < 4; n++) {
        imx8mm_gpc_set_pgc_state(s, GPC_PGC_BASE + n * 0x40, true);
    }
    for (int n = 0; n < GPC_NUM_PU_DOMAINS; n++) {
        imx8mm_gpc_set_pgc_state(s, GPC_PGC_PU_BASE + n * 0x40, true);
    }
}

static void imx8mm_gpc_init(Object *obj)
{
    IMX8MMGPCState *s = IMX8MM_GPC(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_gpc_ops, s,
                          TYPE_IMX8MM_GPC, IMX8MM_GPC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription imx8mm_gpc_vmstate = {
    .name = TYPE_IMX8MM_GPC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMGPCState, IMX8MM_GPC_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx8mm_gpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_gpc_reset);
    dc->vmsd = &imx8mm_gpc_vmstate;
    dc->desc = "i.MX8MM General Power Controller";
}

static const TypeInfo imx8mm_gpc_types[] = {
    {
        .name          = TYPE_IMX8MM_GPC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMGPCState),
        .instance_init = imx8mm_gpc_init,
        .class_init    = imx8mm_gpc_class_init,
    },
};

DEFINE_TYPES(imx8mm_gpc_types)
