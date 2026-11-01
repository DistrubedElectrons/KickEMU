/*
 * KICK peripheral test machine (qtest only)
 *
 * A CPU-less machine that places the KICK I2C controllers, SAI2 and SDMA2
 * at their i.MX 8M Mini addresses so the device models can be exercised by
 * qtests before the full fsl-imx8mm SoC is available. It is not meant to
 * boot anything. The wiring mirrors what the kick board does:
 *
 *   I2C3  0x30a40000  bus "i2c-bus.0"  (GT911 0x14, PCF85263 0x68)
 *   I2C4  0x30a50000  bus "i2c-bus.1"  (CV/Gate DAC 0x0c)
 *   SAI2  0x30020000  dma-rx -> SDMA2 event 2, dma-tx -> SDMA2 event 3
 *   SDMA2 0x302c0000  bus master on system memory
 *   RAM   0x40000000
 *
 * The I2C slaves are added with -device ...,bus=i2c-bus.N. Interrupt
 * outputs are left unconnected; qtests intercept them on the devices
 * (/machine/sai2, /machine/sdma2).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/imx_i2c.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "imx8mm_sai.h"
#include "imx_sdma.h"

/* i.MX 8M Mini memory map (RM "System Memory Map") */
#define KPT_I2C3_BASE       0x30a40000
#define KPT_I2C4_BASE       0x30a50000
#define KPT_SAI2_BASE       0x30020000
#define KPT_SDMA2_BASE      0x302c0000
#define KPT_RAM_BASE        0x40000000

/* SDMA2 event numbers of SAI2 (imx8mm.dtsi: dmas = <&sdma2 2 ..>, <3 ..>) */
#define KPT_SAI2_EVT_RX     2
#define KPT_SAI2_EVT_TX     3

/* SAI2 MCLK1 = SAI2_ROOT, KICK DT assigned-clock-rates */
#define KPT_SAI2_MCLK1_HZ   12288000

static void kick_periph_test_init(MachineState *machine)
{
    static const hwaddr i2c_base[] = { KPT_I2C3_BASE, KPT_I2C4_BASE };
    static const char *const i2c_name[] = { "i2c3", "i2c4" };
    DeviceState *sai, *sdma;
    unsigned i;

    if (machine->ram) {
        memory_region_add_subregion(get_system_memory(), KPT_RAM_BASE,
                                    machine->ram);
    }

    for (i = 0; i < ARRAY_SIZE(i2c_base); i++) {
        DeviceState *dev = qdev_new(TYPE_IMX_I2C);

        object_property_add_child(OBJECT(machine), i2c_name[i], OBJECT(dev));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, i2c_base[i]);
    }

    sdma = qdev_new(TYPE_IMX_SDMA);
    object_property_add_child(OBJECT(machine), "sdma2", OBJECT(sdma));
    object_property_set_link(OBJECT(sdma), "memory",
                             OBJECT(get_system_memory()), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sdma), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sdma), 0, KPT_SDMA2_BASE);

    sai = qdev_new(TYPE_IMX8MM_SAI);
    object_property_add_child(OBJECT(machine), "sai2", OBJECT(sai));
    qdev_prop_set_uint32(sai, "mclk1-hz", KPT_SAI2_MCLK1_HZ);
    if (machine->audiodev) {
        qdev_prop_set_string(sai, "audiodev", machine->audiodev);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sai), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sai), 0, KPT_SAI2_BASE);
    qdev_connect_gpio_out_named(sai, IMX8MM_SAI_GPIO_DMA_RX, 0,
                                qdev_get_gpio_in_named(sdma,
                                                       IMX_SDMA_GPIO_EVENTS,
                                                       KPT_SAI2_EVT_RX));
    qdev_connect_gpio_out_named(sai, IMX8MM_SAI_GPIO_DMA_TX, 0,
                                qdev_get_gpio_in_named(sdma,
                                                       IMX_SDMA_GPIO_EVENTS,
                                                       KPT_SAI2_EVT_TX));
}

static void kick_periph_test_machine_init(MachineClass *mc)
{
    mc->desc = "KICK peripheral test machine (qtest only, no CPU)";
    mc->init = kick_periph_test_init;
    mc->max_cpus = 1;
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "kick-periph-test.ram";
    mc->no_serial = 1;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    machine_add_audiodev_property(mc);
}

DEFINE_MACHINE_AARCH64("kick-periph-test", kick_periph_test_machine_init)
