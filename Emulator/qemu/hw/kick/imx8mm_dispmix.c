/*
 * i.MX8M Mini DISPLAY_BLK_CTRL (display mix GPR, "dispmix")
 *
 * The block sits at 0x32e28000 inside the DISPMIX power domain. The vendor
 * kernel (varigit linux-imx 5.4) drives three registers of it through
 * drivers/reset/reset-dispmix.c:
 *
 *   0x00  DISPLAY_MIX_SFT_RSTN_CSR  "fsl,imx8mm-dispmix-sft-rstn", active low
 *   0x04  DISPLAY_MIX_CLK_EN_CSR    "fsl,imx8mm-dispmix-clk-en",   active high
 *   0x08  GPR_MIPI_RESET_DIV        "fsl,imx8mm-dispmix-mipi-rst", active low,
 *                                   bit 16 MIPI_S_RESET, bit 17 MIPI_M_RESET
 *
 * Bit assignments are those of include/dt-bindings/reset/imx8mm-dispmix.h.
 * The rest of the 256 byte window is the display GPR syscon (lcdif-gpr,
 * dsi-gpr). The 5.4 LCDIF and DSIM drivers never touch it on the 8MM, so it
 * is kept as plain storage and accesses are logged as unimplemented.
 *
 * Every reset and clock-enable bit is also exported as a GPIO output so the
 * SoC can gate the LCDIF and DSIM models if it wants to.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/kick/imx8mm_dispmix.h"
#include "migration/vmstate.h"

#define DISPMIX_SFT_RSTN        0x00
#define DISPMIX_CLK_EN          0x04
#define DISPMIX_MIPI_RESET_DIV  0x08

#define SFT_RSTN_MASK   ((1u << IMX8MM_DISPMIX_NUM_SFT_RSTN) - 1)
#define CLK_EN_MASK     ((1u << IMX8MM_DISPMIX_NUM_CLK_EN) - 1)
#define MIPI_RST_SHIFT  16
#define MIPI_RST_MASK   (((1u << IMX8MM_DISPMIX_NUM_MIPI_RST) - 1) << MIPI_RST_SHIFT)

static void dispmix_update_lines(IMX8MMDispMixState *s)
{
    int i;

    for (i = 0; i < IMX8MM_DISPMIX_NUM_SFT_RSTN; i++) {
        qemu_set_irq(s->sft_rstn[i], (s->regs[DISPMIX_SFT_RSTN / 4] >> i) & 1);
    }
    for (i = 0; i < IMX8MM_DISPMIX_NUM_CLK_EN; i++) {
        qemu_set_irq(s->clk_en[i], (s->regs[DISPMIX_CLK_EN / 4] >> i) & 1);
    }
    for (i = 0; i < IMX8MM_DISPMIX_NUM_MIPI_RST; i++) {
        qemu_set_irq(s->mipi_rst[i],
                     (s->regs[DISPMIX_MIPI_RESET_DIV / 4] >>
                      (MIPI_RST_SHIFT + i)) & 1);
    }
}

static uint64_t dispmix_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMDispMixState *s = opaque;

    switch (offset) {
    case DISPMIX_SFT_RSTN:
    case DISPMIX_CLK_EN:
    case DISPMIX_MIPI_RESET_DIV:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: display GPR read at 0x%" HWADDR_PRIx
                      " has no modelled function\n", __func__, offset);
        break;
    }
    return s->regs[offset / 4];
}

static void dispmix_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    IMX8MMDispMixState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case DISPMIX_SFT_RSTN:
        v &= SFT_RSTN_MASK;
        break;
    case DISPMIX_CLK_EN:
        v &= CLK_EN_MASK;
        break;
    case DISPMIX_MIPI_RESET_DIV:
        /*
         * Bits 15:0 hold the MIPI clock dividers, which have no effect in
         * the model; they are kept so that read-modify-write sequences work.
         */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: display GPR write 0x%08x at 0x%"
                      HWADDR_PRIx " has no modelled function\n",
                      __func__, v, offset);
        break;
    }
    s->regs[offset / 4] = v;
    dispmix_update_lines(s);
}

static const MemoryRegionOps dispmix_ops = {
    .read = dispmix_read,
    .write = dispmix_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void dispmix_reset_hold(Object *obj, ResetType type)
{
    IMX8MMDispMixState *s = IMX8MM_DISPMIX(obj);

    /* After power-up all display mix resets are asserted and clocks gated. */
    memset(s->regs, 0, sizeof(s->regs));
    dispmix_update_lines(s);
}

static void dispmix_init(Object *obj)
{
    IMX8MMDispMixState *s = IMX8MM_DISPMIX(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dispmix_ops, s,
                          TYPE_IMX8MM_DISPMIX, IMX8MM_DISPMIX_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    qdev_init_gpio_out_named(dev, s->sft_rstn, "sft-rstn",
                             IMX8MM_DISPMIX_NUM_SFT_RSTN);
    qdev_init_gpio_out_named(dev, s->clk_en, "clk-en",
                             IMX8MM_DISPMIX_NUM_CLK_EN);
    qdev_init_gpio_out_named(dev, s->mipi_rst, "mipi-rst",
                             IMX8MM_DISPMIX_NUM_MIPI_RST);
}

static const VMStateDescription vmstate_dispmix = {
    .name = TYPE_IMX8MM_DISPMIX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMDispMixState,
                             IMX8MM_DISPMIX_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void dispmix_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "i.MX8MM display mix block control";
    dc->vmsd = &vmstate_dispmix;
    rc->phases.hold = dispmix_reset_hold;
}

static const TypeInfo dispmix_info = {
    .name = TYPE_IMX8MM_DISPMIX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX8MMDispMixState),
    .instance_init = dispmix_init,
    .class_init = dispmix_class_init,
};

static void dispmix_register_types(void)
{
    type_register_static(&dispmix_info);
}

type_init(dispmix_register_types)
