/*
 * i.MX 8M Mini IOMUX Controller (IOMUXC) and IOMUXC_GPR
 *
 * IOMUXC (IMX8MMRM chapter 8.2): SW_MUX_CTL_PAD_* (0x014..0x250),
 * SW_PAD_CTL_PAD_* (0x254..0x4b8) and *_SELECT_INPUT (0x4bc..0x540). Pin
 * routing is not modelled: the board connects device models directly, so
 * the registers are storage that reads back what SPL and the pinctrl
 * driver program, which is what both of them verify.
 *
 * IOMUXC_GPR (chapter 8.2.4): GPR0..GPR22, general purpose SoC control bits
 * (ENET stop mode, MIPI/USB PHY controls, OCRAM/TZASC configuration).
 * Plain storage with reset value 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"

#include "imx8mm_iomuxc.h"

/* The first defined register in IOMUXC is SW_MUX_CTL_PAD_BOOT_MODE2 */
#define IOMUXC_FIRST_REG    0x014

static uint64_t imx8mm_iomuxc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMIOMUXCState *s = IMX8MM_IOMUXC(opaque);

    if (offset < IOMUXC_FIRST_REG || offset / 4 >= IMX8MM_IOMUXC_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_IOMUXC, offset);
        return 0;
    }
    return s->regs[offset / 4];
}

static void imx8mm_iomuxc_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    IMX8MMIOMUXCState *s = IMX8MM_IOMUXC(opaque);

    if (offset < IOMUXC_FIRST_REG || offset / 4 >= IMX8MM_IOMUXC_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_IOMUXC, offset);
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx8mm_iomuxc_ops = {
    .read = imx8mm_iomuxc_read,
    .write = imx8mm_iomuxc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_iomuxc_reset(DeviceState *dev)
{
    IMX8MMIOMUXCState *s = IMX8MM_IOMUXC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx8mm_iomuxc_init(Object *obj)
{
    IMX8MMIOMUXCState *s = IMX8MM_IOMUXC(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_iomuxc_ops, s,
                          TYPE_IMX8MM_IOMUXC, IMX8MM_IOMUXC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription imx8mm_iomuxc_vmstate = {
    .name = TYPE_IMX8MM_IOMUXC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMIOMUXCState, IMX8MM_IOMUXC_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx8mm_iomuxc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_iomuxc_reset);
    dc->vmsd = &imx8mm_iomuxc_vmstate;
    dc->desc = "i.MX8MM IOMUX Controller";
}

static uint64_t imx8mm_iomuxc_gpr_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    IMX8MMIOMUXCGPRState *s = IMX8MM_IOMUXC_GPR(opaque);

    if (offset / 4 >= IMX8MM_IOMUXC_GPR_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_IOMUXC_GPR, offset);
        return 0;
    }
    return s->regs[offset / 4];
}

static void imx8mm_iomuxc_gpr_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    IMX8MMIOMUXCGPRState *s = IMX8MM_IOMUXC_GPR(opaque);

    if (offset / 4 >= IMX8MM_IOMUXC_GPR_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_IOMUXC_GPR, offset);
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx8mm_iomuxc_gpr_ops = {
    .read = imx8mm_iomuxc_gpr_read,
    .write = imx8mm_iomuxc_gpr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_iomuxc_gpr_reset(DeviceState *dev)
{
    IMX8MMIOMUXCGPRState *s = IMX8MM_IOMUXC_GPR(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx8mm_iomuxc_gpr_init(Object *obj)
{
    IMX8MMIOMUXCGPRState *s = IMX8MM_IOMUXC_GPR(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_iomuxc_gpr_ops, s,
                          TYPE_IMX8MM_IOMUXC_GPR, IMX8MM_IOMUXC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription imx8mm_iomuxc_gpr_vmstate = {
    .name = TYPE_IMX8MM_IOMUXC_GPR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMIOMUXCGPRState,
                             IMX8MM_IOMUXC_GPR_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx8mm_iomuxc_gpr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_iomuxc_gpr_reset);
    dc->vmsd = &imx8mm_iomuxc_gpr_vmstate;
    dc->desc = "i.MX8MM IOMUXC general purpose registers";
}

static const TypeInfo imx8mm_iomuxc_types[] = {
    {
        .name          = TYPE_IMX8MM_IOMUXC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMIOMUXCState),
        .instance_init = imx8mm_iomuxc_init,
        .class_init    = imx8mm_iomuxc_class_init,
    },
    {
        .name          = TYPE_IMX8MM_IOMUXC_GPR,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMIOMUXCGPRState),
        .instance_init = imx8mm_iomuxc_gpr_init,
        .class_init    = imx8mm_iomuxc_gpr_class_init,
    },
};

DEFINE_TYPES(imx8mm_iomuxc_types)
