/*
 * i.MX 8M Mini uSDHC: upstream imx-usdhc with the 8MM clock and busy
 * behaviour
 *
 * Two differences between QEMU's generic imx-usdhc and the uSDHC of the
 * i.MX8MM that the boot chain depends on:
 *
 * - SYS_CTRL (0x2c) bits [3:0] are reserved and read as 1 on the uSDHC
 *   (IMX8MMRM 10.3, System Control); the card clock is gated by
 *   VEND_SPEC.FRC_SDCLK_ON and automatic gating instead. The generic SDHCI
 *   core treats those bits as "internal clock / SD clock enable" and
 *   refuses commands while they are 0, which is how U-Boot (SPL and
 *   proper) leaves them; Linux happens to set them. The model forces them
 *   to 1 on every write, as they read on hardware.
 * - The uSDHC raises INT_STATUS.TC when the busy signalling of an R1b
 *   command ends; the 5.4 esdhc driver relies on it for eMMC CMD6
 *   (MMC_CAP_WAIT_WHILE_BUSY). The generic model suppresses that
 *   interrupt (SDHCI_QUIRK_NO_BUSY_IRQ), so the quirk is cleared here.
 * - INT_STATUS_EN (0x34) resets to 0x117f013f (command/transfer complete,
 *   buffer, card and all error status enabled), after power-on and after
 *   SYS_CTRL.RSTA. U-Boot sets RSTA after programming the enables and then
 *   relies on that reset value; the generic model resets it to 0, so no
 *   command ever reports completion.
 * - BLK_ATT has no SDMA buffer boundary field on the uSDHC: a simple DMA
 *   transfer runs to the end without pausing. The generic model forces a
 *   512 KiB boundary for the eSDHC and then waits for the driver to
 *   restart the DMA, which U-Boot never does. When it pauses there, the
 *   model resumes it at the current address before the guest sees the
 *   status (the DMA interrupt does not exist for this case on the uSDHC).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"

#include "imx8mm_usdhc.h"

#define USDHC_DS_ADDR           0x00
#define USDHC_INT_STATUS        0x30
#define USDHC_SYS_CTRL          0x2c
#define SYS_CTRL_RESERVED_ONES  0xf
#define SYS_CTRL_RSTA           (1u << 24)
/* INT_STATUS_EN reset value: normal status 0x013f, error status 0x117f */
#define INT_STATUS_EN_NORMAL    0x013f
#define INT_STATUS_EN_ERROR     0x117f

/* SDHCI core state bits (SD Host Controller spec, as in sdhci-internal.h) */
#define PRNSTS_DATA_INHIBIT     0x0002
#define PRNSTS_DAT_ACTIVE       0x0004
#define PRNSTS_DOING_WRITE      0x0100
#define PRNSTS_DOING_READ       0x0200
#define NIS_DMA                 0x0008
#define HOSTCTL1_DMA_MASK       0x18    /* 0 = SDMA */

static void imx8mm_usdhc_status_enable_reset(SDHCIState *p)
{
    p->norintstsen = INT_STATUS_EN_NORMAL;
    p->errintstsen = INT_STATUS_EN_ERROR;
}

static uint64_t imx8mm_usdhc_sysctl_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    SDHCIState *p = SYSBUS_SDHCI(opaque);

    uint64_t v = p->iomem.ops->read(p->iomem.opaque, USDHC_SYS_CTRL + offset,
                                    size);

    /* the reserved ones live in the lowest byte */
    return offset == 0 ? v | SYS_CTRL_RESERVED_ONES : v;
}

static void imx8mm_usdhc_sysctl_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    SDHCIState *p = SYSBUS_SDHCI(opaque);

    /*
     * Call the upstream handler directly: dispatching through its memory
     * region from here would trip QEMU's re-entrancy guard (same device).
     */
    uint64_t rsta = SYS_CTRL_RSTA >> (offset * 8);

    p->iomem.ops->write(p->iomem.opaque, USDHC_SYS_CTRL + offset,
                        offset == 0 ? value | SYS_CTRL_RESERVED_ONES : value,
                        size);
    if (offset <= 3 && offset + size > 3 && (value & rsta)) {
        imx8mm_usdhc_status_enable_reset(p);
    }
}

static uint64_t imx8mm_usdhc_status_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    SDHCIState *p = SYSBUS_SDHCI(opaque);

    /* Resume a simple DMA transfer the generic core paused at a boundary */
    while ((p->norintsts & NIS_DMA) && p->blkcnt &&
           (p->prnsts & (PRNSTS_DOING_READ | PRNSTS_DOING_WRITE)) &&
           !(p->hostctl1 & HOSTCTL1_DMA_MASK)) {
        p->norintsts &= ~NIS_DMA;
        p->prnsts &= ~(PRNSTS_DOING_READ | PRNSTS_DOING_WRITE |
                       PRNSTS_DATA_INHIBIT | PRNSTS_DAT_ACTIVE);
        p->iomem.ops->write(p->iomem.opaque, USDHC_DS_ADDR, p->sdmasysad, 4);
    }
    return p->iomem.ops->read(p->iomem.opaque, USDHC_INT_STATUS + offset,
                              size);
}

static void imx8mm_usdhc_status_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    SDHCIState *p = SYSBUS_SDHCI(opaque);

    p->iomem.ops->write(p->iomem.opaque, USDHC_INT_STATUS + offset, value,
                        size);
}

static const MemoryRegionOps imx8mm_usdhc_status_ops = {
    .read = imx8mm_usdhc_status_read,
    .write = imx8mm_usdhc_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps imx8mm_usdhc_sysctl_ops = {
    .read = imx8mm_usdhc_sysctl_read,
    .write = imx8mm_usdhc_sysctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void imx8mm_usdhc_init(Object *obj)
{
    IMX8MMUSDHCState *s = IMX8MM_USDHC(obj);
    SDHCIState *p = SYSBUS_SDHCI(obj);

    p->quirks &= ~SDHCI_QUIRK_NO_BUSY_IRQ;
    memory_region_init(&s->container, obj, TYPE_IMX8MM_USDHC,
                       IMX8MM_USDHC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->container);
}

static DeviceRealize imx8mm_usdhc_parent_realize;
static ResettablePhases imx8mm_usdhc_parent_phases;

static void imx8mm_usdhc_reset_hold(Object *obj, ResetType type)
{
    if (imx8mm_usdhc_parent_phases.hold) {
        imx8mm_usdhc_parent_phases.hold(obj, type);
    }
    imx8mm_usdhc_status_enable_reset(SYSBUS_SDHCI(obj));
}

static void imx8mm_usdhc_realize(DeviceState *dev, Error **errp)
{
    IMX8MMUSDHCState *s = IMX8MM_USDHC(dev);
    SDHCIState *p = SYSBUS_SDHCI(dev);
    Error *local_err = NULL;

    imx8mm_usdhc_parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    memory_region_init_alias(&s->parent_alias, OBJECT(dev),
                             TYPE_IMX8MM_USDHC ".regs", &p->iomem, 0,
                             IMX8MM_USDHC_SIZE);
    memory_region_init_io(&s->sysctl, OBJECT(dev), &imx8mm_usdhc_sysctl_ops,
                          p, TYPE_IMX8MM_USDHC ".sys-ctrl", 4);
    memory_region_add_subregion(&s->container, 0, &s->parent_alias);
    memory_region_add_subregion_overlap(&s->container, USDHC_SYS_CTRL,
                                        &s->sysctl, 1);
    memory_region_init_io(&s->status, OBJECT(dev), &imx8mm_usdhc_status_ops,
                          p, TYPE_IMX8MM_USDHC ".int-status", 4);
    memory_region_add_subregion_overlap(&s->container, USDHC_INT_STATUS,
                                        &s->status, 1);
}

static void imx8mm_usdhc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    imx8mm_usdhc_parent_realize = dc->realize;
    dc->realize = imx8mm_usdhc_realize;
    resettable_class_set_parent_phases(RESETTABLE_CLASS(klass), NULL,
                                       imx8mm_usdhc_reset_hold, NULL,
                                       &imx8mm_usdhc_parent_phases);
    dc->desc = "i.MX8MM uSDHC";
}

static const TypeInfo imx8mm_usdhc_types[] = {
    {
        .name          = TYPE_IMX8MM_USDHC,
        .parent        = TYPE_IMX_USDHC,
        .instance_size = sizeof(IMX8MMUSDHCState),
        .instance_init = imx8mm_usdhc_init,
        .class_init    = imx8mm_usdhc_class_init,
    },
};

DEFINE_TYPES(imx8mm_usdhc_types)
