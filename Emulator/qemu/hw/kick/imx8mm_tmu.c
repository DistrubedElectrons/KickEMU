/*
 * i.MX 8M Mini Thermal Monitoring Unit (TMU, version 1)
 *
 * IMX8MMRM chapter 7.2 "Thermal Monitoring Unit (TMU)", single probe:
 *
 *   0x00 TER      EN [31]
 *   0x04 TSR      status
 *   0x08 TIER     interrupt enables: ITTEIE [31], ATTEIE [30], ATCTEIE [29]
 *   0x0c TIDR     interrupt detect, write 1 to clear, same bit layout
 *   0x10 TMHTITR  immediate high threshold: EN [31], TEMP [7:0]
 *   0x14 TMHTATR  average high threshold:   EN [31], TEMP [7:0]
 *   0x18 TMHTACTR average critical threshold: EN [31], TEMP [7:0]
 *   0x1c TSCFGR   sensor calibration (programmed from fuses by firmware)
 *   0x20 TRITSR   immediate temperature [7:0] in degrees C
 *   0x24 TRATSR   average temperature [7:0]
 *   0x28 TASR, 0x2c TTMC  analog/test configuration
 *
 * The die temperature comes from the "temperature" property (degrees C)
 * and can be changed at run time with qom-set to test thermal handling;
 * the immediate and average readings both report it while the monitor is
 * enabled. Linux imx8mm_thermal treats readings below 10 C as not ready.
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
#include "qapi/error.h"
#include "qapi/visitor.h"

#include "imx8mm_tmu.h"

#define TMU_TER         0x00
#define TMU_TSR         0x04
#define TMU_TIER        0x08
#define TMU_TIDR        0x0c
#define TMU_TMHTITR     0x10
#define TMU_TMHTATR     0x14
#define TMU_TMHTACTR    0x18
#define TMU_TRITSR      0x20
#define TMU_TRATSR      0x24

#define TER_EN          BIT(31)
#define THR_EN          BIT(31)
#define TIDR_ITTE       BIT(31)
#define TIDR_ATTE       BIT(30)
#define TIDR_ATCTE      BIT(29)
#define TIDR_MASK       (TIDR_ITTE | TIDR_ATTE | TIDR_ATCTE)

#define R(off)          ((off) / 4)

static uint32_t imx8mm_tmu_reading(IMX8MMTMUState *s)
{
    if (!(s->regs[R(TMU_TER)] & TER_EN)) {
        return 0;
    }
    return (uint32_t)s->temperature & 0xff;
}

static bool imx8mm_tmu_over(IMX8MMTMUState *s, hwaddr thr_reg)
{
    uint32_t thr = s->regs[R(thr_reg)];

    return (thr & THR_EN) && (s->regs[R(TMU_TER)] & TER_EN) &&
           s->temperature > (int32_t)(thr & 0xff);
}

/* Latch threshold events into TIDR and drive the interrupt line */
static void imx8mm_tmu_update(IMX8MMTMUState *s)
{
    if (imx8mm_tmu_over(s, TMU_TMHTITR)) {
        s->regs[R(TMU_TIDR)] |= TIDR_ITTE;
    }
    if (imx8mm_tmu_over(s, TMU_TMHTATR)) {
        s->regs[R(TMU_TIDR)] |= TIDR_ATTE;
    }
    if (imx8mm_tmu_over(s, TMU_TMHTACTR)) {
        s->regs[R(TMU_TIDR)] |= TIDR_ATCTE;
    }
    qemu_set_irq(s->irq,
                 !!(s->regs[R(TMU_TIDR)] & s->regs[R(TMU_TIER)] & TIDR_MASK));
}

static uint64_t imx8mm_tmu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMTMUState *s = IMX8MM_TMU(opaque);

    switch (offset) {
    case TMU_TRITSR:
    case TMU_TRATSR:
        return imx8mm_tmu_reading(s);
    default:
        if (offset < IMX8MM_TMU_NUM_REGS * 4) {
            return s->regs[R(offset)];
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_TMU, offset);
        return 0;
    }
}

static void imx8mm_tmu_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX8MMTMUState *s = IMX8MM_TMU(opaque);

    switch (offset) {
    case TMU_TSR:
    case TMU_TRITSR:
    case TMU_TRATSR:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_TMU, offset);
        return;
    case TMU_TIDR:
        s->regs[R(offset)] &= ~(value & TIDR_MASK);
        break;
    default:
        if (offset >= IMX8MM_TMU_NUM_REGS * 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%"
                          HWADDR_PRIx "\n", TYPE_IMX8MM_TMU, offset);
            return;
        }
        s->regs[R(offset)] = value;
        break;
    }
    imx8mm_tmu_update(s);
}

static const MemoryRegionOps imx8mm_tmu_ops = {
    .read = imx8mm_tmu_read,
    .write = imx8mm_tmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_tmu_get_temperature(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    IMX8MMTMUState *s = IMX8MM_TMU(obj);

    visit_type_int32(v, name, &s->temperature, errp);
}

static void imx8mm_tmu_set_temperature(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    IMX8MMTMUState *s = IMX8MM_TMU(obj);
    int32_t temp;

    if (!visit_type_int32(v, name, &temp, errp)) {
        return;
    }
    /* The 8-bit result register covers 0..125 C on the 8MM */
    if (temp < 0 || temp > 125) {
        error_setg(errp, "temperature %d C outside 0..125", temp);
        return;
    }
    s->temperature = temp;
    imx8mm_tmu_update(s);
}

static void imx8mm_tmu_reset(DeviceState *dev)
{
    IMX8MMTMUState *s = IMX8MM_TMU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    imx8mm_tmu_update(s);
}

static void imx8mm_tmu_init(Object *obj)
{
    IMX8MMTMUState *s = IMX8MM_TMU(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_tmu_ops, s,
                          TYPE_IMX8MM_TMU, IMX8MM_TMU_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    /* A plausible idle die temperature for a passively cooled module */
    s->temperature = 45;
    object_property_add(obj, "temperature", "int32",
                        imx8mm_tmu_get_temperature,
                        imx8mm_tmu_set_temperature, NULL, NULL);
}

static const VMStateDescription imx8mm_tmu_vmstate = {
    .name = TYPE_IMX8MM_TMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMTMUState, IMX8MM_TMU_NUM_REGS),
        VMSTATE_INT32(temperature, IMX8MMTMUState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx8mm_tmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_tmu_reset);
    dc->vmsd = &imx8mm_tmu_vmstate;
    dc->desc = "i.MX8MM Thermal Monitoring Unit";
}

static const TypeInfo imx8mm_tmu_types[] = {
    {
        .name          = TYPE_IMX8MM_TMU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMTMUState),
        .instance_init = imx8mm_tmu_init,
        .class_init    = imx8mm_tmu_class_init,
    },
};

DEFINE_TYPES(imx8mm_tmu_types)
