/*
 * i.MX 8M Mini uSDHC command queue engine (CQHCI) register block
 *
 * The i.MX8MM uSDHC integrates an eMMC 5.1 command queue host controller
 * (JESD84-B51 appendix B) at base + 0x100. The Linux esdhc driver probes it
 * on every instance with ESDHC_FLAG_CQHCI (cqhci_init reads CQVER), and only
 * enables queueing when the card reports CMDQ support in EXT_CSD. QEMU's
 * eMMC model reports EXT_CSD revision 5 without CMDQ, so the engine stays
 * idle; this model provides the register block with the semantics the
 * driver relies on while idle:
 *
 *   0x00 CQVER     version (read-only)
 *   0x04 CQCAP     capabilities (read-only)
 *   0x08 CQCFG     CQ_ENABLE [0], TASK_DESC_SZ [8], DCMD [12]
 *   0x0c CQCTL     HALT [0], CLEAR_ALL_TASKS [8] (self-clearing)
 *   0x10 CQIS      HAC [0], TCC [1], RED [2], TCL [3]; write 1 to clear
 *   0x14 CQISTE, 0x18 CQISGE   status / signal enables
 *   0x1c CQIC      interrupt coalescing
 *   0x20/0x24 CQTDLBA/U  task descriptor list base
 *   0x28 CQTDBR    task doorbell, 0x2c CQTCN task completion (W1C)
 *   0x30 CQDQS, 0x34 CQDPT, 0x38 CQTCLR, 0x40 CQSSC1, 0x44 CQSSC2,
 *   0x48 CQCRDCT, 0x50 CQRMEM, 0x54 CQTERRI, 0x58 CQCRI, 0x5c CQCRA
 *
 * Setting HALT completes at once and raises CQIS.HAC. Task execution is
 * not implemented: a doorbell write is logged as unimplemented.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#include "imx8mm_cqhci.h"

#define CQVER       0x00
#define CQCAP       0x04
#define CQCTL       0x0c
#define CQIS        0x10
#define CQISTE      0x14
#define CQISGE      0x18
#define CQTDBR      0x28
#define CQTCN       0x2c
#define CQTCLR      0x38
#define CQTERRI     0x54
#define CQCRI       0x58
#define CQCRA       0x5c

#define CQCTL_HALT          BIT(0)
#define CQCTL_CLEAR_ALL     BIT(8)
#define CQIS_HAC            BIT(0)
#define CQIS_MASK           0x0f

#define R(off)      ((off) / 4)

static void imx8mm_cqhci_update_irq(IMX8MMCQHCIState *s)
{
    uint32_t pending = s->regs[R(CQIS)] & s->regs[R(CQISTE)] &
                       s->regs[R(CQISGE)];

    qemu_set_irq(s->irq, !!pending);
}

static uint64_t imx8mm_cqhci_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMCQHCIState *s = IMX8MM_CQHCI(opaque);

    if (offset >= IMX8MM_CQHCI_NUM_REGS * 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_CQHCI, offset);
        return 0;
    }
    return s->regs[R(offset)];
}

static void imx8mm_cqhci_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX8MMCQHCIState *s = IMX8MM_CQHCI(opaque);

    switch (offset) {
    case CQVER:
    case CQCAP:
    case CQTERRI:
    case CQCRI:
    case CQCRA:
        return;     /* read-only */
    case CQCTL:
        /* HALT takes effect immediately; CLEAR_ALL_TASKS self-clears */
        if ((value & CQCTL_HALT) && !(s->regs[R(CQCTL)] & CQCTL_HALT)) {
            s->regs[R(CQIS)] |= CQIS_HAC;
        }
        s->regs[R(CQCTL)] = value & CQCTL_HALT;
        if (value & CQCTL_CLEAR_ALL) {
            s->regs[R(CQTDBR)] = 0;
        }
        break;
    case CQIS:
    case CQTCN:
        s->regs[R(offset)] &= ~value;   /* W1C */
        break;
    case CQTDBR:
        if (value) {
            qemu_log_mask(LOG_UNIMP, "%s: command queue task execution "
                          "(doorbell 0x%" PRIx64 ") not implemented\n",
                          TYPE_IMX8MM_CQHCI, value);
        }
        break;
    case CQTCLR:
        s->regs[R(CQTDBR)] &= ~value;
        break;
    default:
        if (offset >= IMX8MM_CQHCI_NUM_REGS * 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%"
                          HWADDR_PRIx "\n", TYPE_IMX8MM_CQHCI, offset);
            return;
        }
        s->regs[R(offset)] = value;
        break;
    }
    imx8mm_cqhci_update_irq(s);
}

static const MemoryRegionOps imx8mm_cqhci_ops = {
    .read = imx8mm_cqhci_read,
    .write = imx8mm_cqhci_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_cqhci_reset(DeviceState *dev)
{
    IMX8MMCQHCIState *s = IMX8MM_CQHCI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R(CQVER)] = s->version;
    s->regs[R(CQCAP)] = s->capabilities;
    imx8mm_cqhci_update_irq(s);
}

static void imx8mm_cqhci_init(Object *obj)
{
    IMX8MMCQHCIState *s = IMX8MM_CQHCI(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_cqhci_ops, s,
                          TYPE_IMX8MM_CQHCI, IMX8MM_CQHCI_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription imx8mm_cqhci_vmstate = {
    .name = TYPE_IMX8MM_CQHCI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMCQHCIState, IMX8MM_CQHCI_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx8mm_cqhci_properties[] = {
    /* CQHCI 5.10 as specified by eMMC 5.1 */
    DEFINE_PROP_UINT32("version", IMX8MMCQHCIState, version, 0x00000510),
    DEFINE_PROP_UINT32("capabilities", IMX8MMCQHCIState, capabilities, 0),
};

static void imx8mm_cqhci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_cqhci_reset);
    device_class_set_props(dc, imx8mm_cqhci_properties);
    dc->vmsd = &imx8mm_cqhci_vmstate;
    dc->desc = "i.MX8MM uSDHC command queue engine";
}

static const TypeInfo imx8mm_cqhci_types[] = {
    {
        .name          = TYPE_IMX8MM_CQHCI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMCQHCIState),
        .instance_init = imx8mm_cqhci_init,
        .class_init    = imx8mm_cqhci_class_init,
    },
};

DEFINE_TYPES(imx8mm_cqhci_types)
