/*
 * i.MX 8M Mini I2C: upstream imx_i2c with the byte-complete flag on NACK
 *
 * On the i.MX I2C controller I2SR.IIF is set at the falling edge of the
 * ninth clock of every byte transfer, acknowledged or not; I2SR.RXAK then
 * tells the two apart (IMX8MMRM 16.1, I2C Status Register). The Linux
 * i2c-imx driver waits for IIF after every byte and only then checks RXAK.
 * QEMU's imx_i2c leaves IIF clear when the address or a data byte is not
 * acknowledged, so a probe of an absent device times out (-ETIMEDOUT
 * after the driver's timeout) instead of failing with -ENXIO at once.
 *
 * This subclass forwards every access to the upstream model and, after a
 * master-mode I2DR write that ended with RXAK set, raises IIF (and the
 * interrupt, when IIEN is set) as the hardware does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"

#include "imx8mm_i2c.h"

#define I2C_REGION_SIZE     0x10000

static uint64_t imx8mm_i2c_i2dr_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    IMXI2CState *p = IMX_I2C(opaque);

    return p->iomem.ops->read(p->iomem.opaque, I2DR_ADDR, size);
}

static void imx8mm_i2c_i2dr_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    IMXI2CState *p = IMX_I2C(opaque);
    bool master_tx = (p->i2cr & (I2CR_IEN | I2CR_MSTA)) ==
                     (I2CR_IEN | I2CR_MSTA);

    /*
     * Call the upstream handler directly: dispatching through its memory
     * region from here would trip QEMU's re-entrancy guard (same device).
     */
    p->iomem.ops->write(p->iomem.opaque, I2DR_ADDR, value, size);

    if (master_tx && (p->i2sr & I2SR_RXAK) && !(p->i2sr & I2SR_IIF)) {
        /* NACK: the byte transfer still completed */
        p->i2sr |= I2SR_IIF;
        if (p->i2cr & I2CR_IIEN) {
            qemu_irq_raise(p->irq);
        }
    }
}

static const MemoryRegionOps imx8mm_i2c_i2dr_ops = {
    .read = imx8mm_i2c_i2dr_read,
    .write = imx8mm_i2c_i2dr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
};

static void imx8mm_i2c_init(Object *obj)
{
    IMX8MMI2CState *s = IMX8MM_I2C(obj);

    /*
     * The upstream model registers its registers at realize, so this
     * window is sysbus MMIO 0 and the parent's own region follows as 1.
     */
    memory_region_init(&s->container, obj, TYPE_IMX8MM_I2C, I2C_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->container);
}

static DeviceRealize imx8mm_i2c_parent_realize;

static void imx8mm_i2c_realize(DeviceState *dev, Error **errp)
{
    IMX8MMI2CState *s = IMX8MM_I2C(dev);
    IMXI2CState *p = IMX_I2C(dev);
    Error *local_err = NULL;

    imx8mm_i2c_parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    /* The upstream registers, with I2DR replaced by the forwarding one */
    memory_region_init_alias(&s->parent_alias, OBJECT(dev),
                             TYPE_IMX8MM_I2C ".regs", &p->iomem, 0,
                             IMX_I2C_MEM_SIZE);
    memory_region_init_io(&s->i2dr, OBJECT(dev), &imx8mm_i2c_i2dr_ops, p,
                          TYPE_IMX8MM_I2C ".i2dr", 2);
    memory_region_add_subregion(&s->container, 0, &s->parent_alias);
    memory_region_add_subregion_overlap(&s->container, I2DR_ADDR, &s->i2dr,
                                        1);
}

static void imx8mm_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    imx8mm_i2c_parent_realize = dc->realize;
    dc->realize = imx8mm_i2c_realize;
    dc->desc = "i.MX8MM I2C controller";
}

static const TypeInfo imx8mm_i2c_types[] = {
    {
        .name          = TYPE_IMX8MM_I2C,
        .parent        = TYPE_IMX_I2C,
        .instance_size = sizeof(IMX8MMI2CState),
        .instance_init = imx8mm_i2c_init,
        .class_init    = imx8mm_i2c_class_init,
    },
};

DEFINE_TYPES(imx8mm_i2c_types)
