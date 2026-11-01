/*
 * KICK display/GPU bench machine
 *
 * A machine for qtests that maps the display path and GPU models at
 * their i.MX8M Mini addresses on top of RAM at 0x40000000. It has a single
 * Cortex-A53 only because qtest accesses memory through the first CPU's
 * address space; no firmware is loaded. It exists so the
 * models can be tested on their own; the real wiring lives in the fsl-imx8mm
 * SoC. Interrupt outputs are left unconnected here, tests observe them with
 * qtest_irq_intercept_out_named(..., "sysbus-irq").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/arm/machines-qom.h"
#include "hw/kick/imx8mm_dispmix.h"
#include "hw/i2c/imx_i2c.h"
#include "hw/kick/imx8mm_lcdif.h"
#include "hw/kick/imx8mm_mipi_dsim.h"
#include "hw/kick/sn65dsi83.h"
#include "hw/kick/vivante_gpu.h"
#include "system/address-spaces.h"

/* i.MX8MM memory map (RM chapter 2.1, imx8mm.dtsi). */
#define BENCH_RAM_BASE          0x40000000ULL
#define BENCH_LCDIF_BASE        0x32e00000
#define BENCH_DSIM_BASE         0x32e10000
#define BENCH_DISPMIX_BASE      0x32e28000
#define BENCH_GPU3D_BASE        0x38000000
#define BENCH_GPU2D_BASE        0x38008000
#define BENCH_I2C1_BASE         0x30a20000
#define BENCH_SN65DSI83_ADDR    0x2c

#define BENCH_CPU_TYPE          "cortex-a53-arm-cpu"

static void bench_map(DeviceState *dev, hwaddr base)
{
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
}

static void kick_display_bench_init(MachineState *ms)
{
    DeviceState *dev, *dsim;
    I2CSlave *i2c;

    cpu_create(ms->cpu_type);
    memory_region_add_subregion(get_system_memory(), BENCH_RAM_BASE, ms->ram);

    dev = qdev_new(TYPE_IMX8MM_DISPMIX);
    object_property_add_child(OBJECT(ms), "dispmix", OBJECT(dev));
    bench_map(dev, BENCH_DISPMIX_BASE);

    dev = qdev_new(TYPE_IMX8MM_LCDIF);
    object_property_add_child(OBJECT(ms), "lcdif", OBJECT(dev));
    bench_map(dev, BENCH_LCDIF_BASE);

    dsim = qdev_new(TYPE_IMX8MM_MIPI_DSIM);
    object_property_add_child(OBJECT(ms), "dsim", OBJECT(dsim));
    bench_map(dsim, BENCH_DSIM_BASE);

    /* The SN65DSI84 sits on I2C1 of the VAR-SOM, as in the board DT. */
    dev = qdev_new(TYPE_IMX_I2C);
    object_property_add_child(OBJECT(ms), "i2c1", OBJECT(dev));
    bench_map(dev, BENCH_I2C1_BASE);
    i2c = i2c_slave_new(TYPE_SN65DSI83, BENCH_SN65DSI83_ADDR);
    object_property_set_link(OBJECT(i2c), "dsi-host", OBJECT(dsim),
                             &error_fatal);
    object_property_add_child(OBJECT(ms), "sn65dsi83", OBJECT(i2c));
    i2c_slave_realize_and_unref(i2c, IMX_I2C(dev)->bus, &error_fatal);

    dev = qdev_new(TYPE_IMX8MM_GPU3D);
    object_property_add_child(OBJECT(ms), "gpu3d", OBJECT(dev));
    bench_map(dev, BENCH_GPU3D_BASE);

    dev = qdev_new(TYPE_IMX8MM_GPU2D);
    object_property_add_child(OBJECT(ms), "gpu2d", OBJECT(dev));
    bench_map(dev, BENCH_GPU2D_BASE);
}

static void kick_display_bench_machine_init(MachineClass *mc)
{
    mc->desc = "KICK display/GPU model bench (qtest only)";
    mc->default_cpu_type = BENCH_CPU_TYPE;
    mc->init = kick_display_bench_init;
    mc->max_cpus = 1;
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "ram";
    mc->no_serial = 1;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE_WITH_INTERFACES("kick-display-bench",
                               kick_display_bench_machine_init,
                               { TYPE_TARGET_AARCH64_MACHINE },
                               { })
