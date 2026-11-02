/*
 * i.MX 8M Mini APBH-Bridge-DMA (GPMI DMA): control registers only
 *
 * The block at 0x33000000 ("fsl,imx7d-dma-apbh", driver mxs-dma) serves
 * the GPMI NAND controller, which the KICK does not use. mxs-dma still
 * probes it at boot and fails with -ETIMEDOUT when the module reset does
 * not complete (stmp_reset_block() in lib/stmp_device.c), which leaves an
 * error in every boot log. This model gives the register file the
 * behaviour that reset needs and stores everything else; no DMA channel
 * ever runs, so the interrupt (SPI 12) is never raised.
 *
 * Every register occupies a 16-byte slot with the usual MXS set (+4),
 * clear (+8) and toggle (+0xc) aliases. CTRL0 bit 31 is SFTRST and bit 30
 * CLKGATE: setting SFTRST resets the block, and the hardware gates the
 * clock once the reset has happened, which stmp_reset_block() polls for.
 * Both bits are set after power-on, as on all MXS-style blocks. The
 * VERSION register (0x800) is not modelled: its value for this SoC was not
 * available, the 5.4 driver does not read it, and it reads 0 like storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "imx8mm_apbh_dma.h"

#define APBH_CTRL0              0x000
#define CTRL0_SFTRST            (1u << 31)
#define CTRL0_CLKGATE           (1u << 30)

static void apbh_block_reset(IMX8MMApbhDmaState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t apbh_read(void *opaque, hwaddr addr, unsigned size)
{
    IMX8MMApbhDmaState *s = opaque;

    return s->regs[addr / 16];
}

static void apbh_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IMX8MMApbhDmaState *s = opaque;
    uint32_t *r = &s->regs[addr / 16];
    uint32_t old = *r;

    switch (addr & 0xc) {
    case 0x0:
        *r = val;
        break;
    case 0x4:
        *r |= val;
        break;
    case 0x8:
        *r &= ~val;
        break;
    default:
        *r ^= val;
        break;
    }
    if (addr / 16 == APBH_CTRL0 / 16 && (*r & CTRL0_SFTRST) &&
        !(old & CTRL0_SFTRST)) {
        /* Soft reset: the block resets, then gates its clock */
        apbh_block_reset(s);
        s->regs[0] = CTRL0_SFTRST | CTRL0_CLKGATE;
    }
}

static const MemoryRegionOps apbh_ops = {
    .read = apbh_read,
    .write = apbh_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void apbh_reset_hold(Object *obj, ResetType type)
{
    IMX8MMApbhDmaState *s = IMX8MM_APBH_DMA(obj);

    apbh_block_reset(s);
    s->regs[0] = CTRL0_SFTRST | CTRL0_CLKGATE;
}

static void apbh_init(Object *obj)
{
    IMX8MMApbhDmaState *s = IMX8MM_APBH_DMA(obj);

    memory_region_init_io(&s->iomem, obj, &apbh_ops, s, TYPE_IMX8MM_APBH_DMA,
                          IMX8MM_APBH_DMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_apbh = {
    .name = TYPE_IMX8MM_APBH_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMApbhDmaState,
                             IMX8MM_APBH_DMA_SIZE / 16),
        VMSTATE_END_OF_LIST()
    }
};

static void apbh_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    RESETTABLE_CLASS(klass)->phases.hold = apbh_reset_hold;
    dc->vmsd = &vmstate_apbh;
    dc->desc = "i.MX8MM APBH DMA (registers only)";
}

static const TypeInfo apbh_types[] = {
    {
        .name          = TYPE_IMX8MM_APBH_DMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMApbhDmaState),
        .instance_init = apbh_init,
        .class_init    = apbh_class_init,
    },
};

DEFINE_TYPES(apbh_types)
