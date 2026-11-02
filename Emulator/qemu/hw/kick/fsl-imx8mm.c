/*
 * NXP i.MX 8M Mini SoC
 *
 * Structure follows QEMU's fsl-imx8mp.c. Memory map and interrupt numbers
 * are the i.MX8MM ones (IMX8MMRM chapters 2 and 3, imx8mm.dtsi). Blocks
 * that have a model are instantiated here; every other window in the
 * memory map is an unimplemented-device so guest accesses are logged with
 * "-d unimp".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "hw/arm/bsa.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"

#include "fsl-imx8mm.h"

const FslImx8mmMemMapEntry fsl_imx8mm_memmap[FSL_IMX8MM_NUM_MEMORY_REGIONS] = {
    [FSL_IMX8MM_BOOT_ROM]           = { 0x00000000, 252 * KiB, "boot_rom" },
    [FSL_IMX8MM_BOOT_ROM_PROTECTED] = { 0x0003f000, 4 * KiB, "boot_rom_protected" },
    [FSL_IMX8MM_CAAM_MEM]           = { 0x00100000, 32 * KiB, "caam_mem" },
    [FSL_IMX8MM_OCRAM_S]            = { 0x00180000, 32 * KiB, "ocram_s" },
    [FSL_IMX8MM_TCM_ITCM]           = { 0x007e0000, 128 * KiB, "tcm_itcm" },
    [FSL_IMX8MM_TCM_DTCM]           = { 0x00800000, 128 * KiB, "tcm_dtcm" },
    [FSL_IMX8MM_OCRAM]              = { FSL_IMX8MM_OCRAM_START,
                                        FSL_IMX8MM_OCRAM_SIZE, "ocram" },
    [FSL_IMX8MM_QSPI_MEM]           = { 0x08000000, 256 * MiB, "qspi_mem" },
    [FSL_IMX8MM_PCIE1_MEM]          = { 0x18000000, 128 * MiB, "pcie1_mem" },
    [FSL_IMX8MM_A53_DAP]            = { 0x28000000, 16 * MiB, "a53_dap" },

    /* AIPS-1 (0x3000_0000 - 0x303f_ffff) */
    [FSL_IMX8MM_SAI1]               = { 0x30010000, 64 * KiB, "sai1" },
    [FSL_IMX8MM_SAI2]               = { 0x30020000, 64 * KiB, "sai2" },
    [FSL_IMX8MM_SAI3]               = { 0x30030000, 64 * KiB, "sai3" },
    [FSL_IMX8MM_SAI5]               = { 0x30050000, 64 * KiB, "sai5" },
    [FSL_IMX8MM_SAI6]               = { 0x30060000, 64 * KiB, "sai6" },
    [FSL_IMX8MM_MICFIL]             = { 0x30080000, 64 * KiB, "micfil" },
    [FSL_IMX8MM_SPDIF1]             = { 0x30090000, 64 * KiB, "spdif1" },
    [FSL_IMX8MM_SPBA2]              = { 0x300f0000, 64 * KiB, "spba2" },
    [FSL_IMX8MM_AIPS1_CONFIGURATION] = { 0x301f0000, 64 * KiB, "aips1_configuration" },
    [FSL_IMX8MM_GPIO1]              = { 0x30200000, 64 * KiB, "gpio1" },
    [FSL_IMX8MM_GPIO2]              = { 0x30210000, 64 * KiB, "gpio2" },
    [FSL_IMX8MM_GPIO3]              = { 0x30220000, 64 * KiB, "gpio3" },
    [FSL_IMX8MM_GPIO4]              = { 0x30230000, 64 * KiB, "gpio4" },
    [FSL_IMX8MM_GPIO5]              = { 0x30240000, 64 * KiB, "gpio5" },
    [FSL_IMX8MM_TMU]                = { 0x30260000, 64 * KiB, "tmu" },
    [FSL_IMX8MM_ANA_OSC]            = { 0x30270000, 64 * KiB, "ana_osc" },
    [FSL_IMX8MM_WDOG1]              = { 0x30280000, 64 * KiB, "wdog1" },
    [FSL_IMX8MM_WDOG2]              = { 0x30290000, 64 * KiB, "wdog2" },
    [FSL_IMX8MM_WDOG3]              = { 0x302a0000, 64 * KiB, "wdog3" },
    [FSL_IMX8MM_SDMA3]              = { 0x302b0000, 64 * KiB, "sdma3" },
    [FSL_IMX8MM_SDMA2]              = { 0x302c0000, 64 * KiB, "sdma2" },
    [FSL_IMX8MM_GPT1]               = { 0x302d0000, 64 * KiB, "gpt1" },
    [FSL_IMX8MM_GPT2]               = { 0x302e0000, 64 * KiB, "gpt2" },
    [FSL_IMX8MM_GPT3]               = { 0x302f0000, 64 * KiB, "gpt3" },
    [FSL_IMX8MM_ROMCP]              = { 0x30310000, 64 * KiB, "romcp" },
    [FSL_IMX8MM_IOMUXC]             = { 0x30330000, 64 * KiB, "iomuxc" },
    [FSL_IMX8MM_IOMUXC_GPR]         = { 0x30340000, 64 * KiB, "iomuxc_gpr" },
    [FSL_IMX8MM_OCOTP_CTRL]         = { 0x30350000, 64 * KiB, "ocotp_ctrl" },
    [FSL_IMX8MM_ANATOP]             = { 0x30360000, 64 * KiB, "anatop" },
    [FSL_IMX8MM_SNVS_HP]            = { 0x30370000, 64 * KiB, "snvs_hp" },
    [FSL_IMX8MM_CCM]                = { 0x30380000, 64 * KiB, "ccm" },
    [FSL_IMX8MM_SRC]                = { 0x30390000, 64 * KiB, "src" },
    [FSL_IMX8MM_GPC]                = { 0x303a0000, 64 * KiB, "gpc" },
    [FSL_IMX8MM_SEMAPHORE1]         = { 0x303b0000, 64 * KiB, "semaphore1" },
    [FSL_IMX8MM_SEMAPHORE2]         = { 0x303c0000, 64 * KiB, "semaphore2" },
    [FSL_IMX8MM_RDC]                = { 0x303d0000, 64 * KiB, "rdc" },
    [FSL_IMX8MM_CSU]                = { 0x303e0000, 64 * KiB, "csu" },

    /* AIPS-2 (0x3040_0000 - 0x307f_ffff) */
    [FSL_IMX8MM_AIPS2_CONFIGURATION] = { 0x305f0000, 64 * KiB, "aips2_configuration" },
    [FSL_IMX8MM_PWM1]               = { 0x30660000, 64 * KiB, "pwm1" },
    [FSL_IMX8MM_PWM2]               = { 0x30670000, 64 * KiB, "pwm2" },
    [FSL_IMX8MM_PWM3]               = { 0x30680000, 64 * KiB, "pwm3" },
    [FSL_IMX8MM_PWM4]               = { 0x30690000, 64 * KiB, "pwm4" },
    [FSL_IMX8MM_SYSCNT_RD]          = { 0x306a0000, 64 * KiB, "syscnt_rd" },
    [FSL_IMX8MM_SYSCNT_CMP]         = { 0x306b0000, 64 * KiB, "syscnt_cmp" },
    [FSL_IMX8MM_SYSCNT_CTRL]        = { 0x306c0000, 64 * KiB, "syscnt_ctrl" },
    [FSL_IMX8MM_GPT6]               = { 0x306e0000, 64 * KiB, "gpt6" },
    [FSL_IMX8MM_GPT5]               = { 0x306f0000, 64 * KiB, "gpt5" },
    [FSL_IMX8MM_GPT4]               = { 0x30700000, 64 * KiB, "gpt4" },
    [FSL_IMX8MM_PERFMON1]           = { 0x307c0000, 64 * KiB, "perfmon1" },
    [FSL_IMX8MM_PERFMON2]           = { 0x307d0000, 64 * KiB, "perfmon2" },
    [FSL_IMX8MM_QOSC]               = { 0x307f0000, 64 * KiB, "qosc" },

    /* AIPS-3 (0x3080_0000 - 0x30bf_ffff) */
    [FSL_IMX8MM_ECSPI1]             = { 0x30820000, 64 * KiB, "ecspi1" },
    [FSL_IMX8MM_ECSPI2]             = { 0x30830000, 64 * KiB, "ecspi2" },
    [FSL_IMX8MM_ECSPI3]             = { 0x30840000, 64 * KiB, "ecspi3" },
    [FSL_IMX8MM_UART1]              = { 0x30860000, 64 * KiB, "uart1" },
    [FSL_IMX8MM_UART3]              = { 0x30880000, 64 * KiB, "uart3" },
    [FSL_IMX8MM_UART2]              = { 0x30890000, 64 * KiB, "uart2" },
    [FSL_IMX8MM_SPBA1]              = { 0x308f0000, 64 * KiB, "spba1" },
    [FSL_IMX8MM_CAAM]               = { 0x30900000, 256 * KiB, "caam" },
    [FSL_IMX8MM_AIPS3_CONFIGURATION] = { 0x309f0000, 64 * KiB, "aips3_configuration" },
    [FSL_IMX8MM_I2C1]               = { 0x30a20000, 64 * KiB, "i2c1" },
    [FSL_IMX8MM_I2C2]               = { 0x30a30000, 64 * KiB, "i2c2" },
    [FSL_IMX8MM_I2C3]               = { 0x30a40000, 64 * KiB, "i2c3" },
    [FSL_IMX8MM_I2C4]               = { 0x30a50000, 64 * KiB, "i2c4" },
    [FSL_IMX8MM_UART4]              = { 0x30a60000, 64 * KiB, "uart4" },
    [FSL_IMX8MM_MU_A]               = { 0x30aa0000, 64 * KiB, "mu_a" },
    [FSL_IMX8MM_MU_B]               = { 0x30ab0000, 64 * KiB, "mu_b" },
    [FSL_IMX8MM_SEMAPHORE_HS]       = { 0x30ac0000, 64 * KiB, "semaphore_hs" },
    [FSL_IMX8MM_USDHC1]             = { 0x30b40000, 64 * KiB, "usdhc1" },
    [FSL_IMX8MM_USDHC2]             = { 0x30b50000, 64 * KiB, "usdhc2" },
    [FSL_IMX8MM_USDHC3]             = { 0x30b60000, 64 * KiB, "usdhc3" },
    [FSL_IMX8MM_QSPI]               = { 0x30bb0000, 64 * KiB, "qspi" },
    [FSL_IMX8MM_SDMA1]              = { 0x30bd0000, 64 * KiB, "sdma1" },
    [FSL_IMX8MM_ENET1]              = { 0x30be0000, 64 * KiB, "enet1" },

    /* NOC and AIPS-4 (0x32c0_0000 - 0x32ff_ffff) */
    [FSL_IMX8MM_NOC]                = { 0x32700000, 1 * MiB, "noc" },
    [FSL_IMX8MM_AIPS4_CONFIGURATION] = { 0x32df0000, 64 * KiB, "aips4_configuration" },
    [FSL_IMX8MM_LCDIF]              = { 0x32e00000, 64 * KiB, "lcdif" },
    [FSL_IMX8MM_MIPI_DSI]           = { 0x32e10000, 64 * KiB, "mipi_dsi" },
    [FSL_IMX8MM_CSI1]               = { 0x32e20000, 32 * KiB, "csi1" },
    [FSL_IMX8MM_DISP_BLK_CTRL]      = { 0x32e28000, 32 * KiB, "disp_blk_ctrl" },
    [FSL_IMX8MM_MIPI_CSI]           = { 0x32e30000, 64 * KiB, "mipi_csi" },
    [FSL_IMX8MM_USB1]               = { 0x32e40000, 64 * KiB, "usb1" },
    [FSL_IMX8MM_USB2]               = { 0x32e50000, 64 * KiB, "usb2" },
    [FSL_IMX8MM_PCIE_PHY1]          = { 0x32f00000, 64 * KiB, "pcie_phy1" },
    [FSL_IMX8MM_TZASC]              = { 0x32f80000, 64 * KiB, "tzasc" },

    [FSL_IMX8MM_APBH_DMA]           = { 0x33000000, 8 * KiB, "apbh_dma" },
    [FSL_IMX8MM_GPMI]               = { 0x33002000, 24 * KiB, "gpmi" },
    [FSL_IMX8MM_PCIE1]              = { 0x33800000, 4 * MiB, "pcie1" },
    [FSL_IMX8MM_GPU3D]              = { 0x38000000, 32 * KiB, "gpu3d" },
    [FSL_IMX8MM_GPU2D]              = { 0x38008000, 32 * KiB, "gpu2d" },
    [FSL_IMX8MM_VPU_G1]             = { 0x38300000, 64 * KiB, "vpu_g1" },
    [FSL_IMX8MM_VPU_G2]             = { 0x38310000, 64 * KiB, "vpu_g2" },
    [FSL_IMX8MM_VPU_H1]             = { 0x38320000, 64 * KiB, "vpu_h1" },
    [FSL_IMX8MM_VPU_BLK_CTRL]       = { 0x38330000, 64 * KiB, "vpu_blk_ctrl" },
    [FSL_IMX8MM_GIC_DIST]           = { 0x38800000, 64 * KiB, "gic_dist" },
    [FSL_IMX8MM_GIC_REDIST]         = { 0x38880000, 512 * KiB, "gic_redist" },
    [FSL_IMX8MM_DDR_PHY]            = { 0x3c000000, 16 * MiB, "ddr_phy" },
    [FSL_IMX8MM_DDR_BLK_CTRL]       = { 0x3d000000, 1 * MiB, "ddr_blk_ctrl" },
    [FSL_IMX8MM_DDR_CTL]            = { 0x3d400000, 4 * MiB, "ddr_ctl" },
    [FSL_IMX8MM_DDR_PERF_MON]       = { 0x3d800000, 4 * MiB, "ddr_perf_mon" },
    [FSL_IMX8MM_RAM]                = { FSL_IMX8MM_RAM_START,
                                        FSL_IMX8MM_RAM_SIZE_MAX, "ram" },
};

#define MMAP(id)    (fsl_imx8mm_memmap[FSL_IMX8MM_##id].addr)

/* DISPMIX SFT_RSTN bit that drives MIPI_DSI_I_PRESET */
#define FSL_IMX8MM_DISPMIX_RST_DSI_PRESET   5

/*
 * Optional peripherals: model name, instance name, memory map entry of
 * MMIO region 0 (and 1), and up to two interrupts (-1 = none). SAI5 and
 * SAI6 share SPI 90 through an OR gate.
 */
typedef struct FslImx8mmOptional {
    const char *type;
    const char *name;
    int mmio[2];
    int irq[2];
} FslImx8mmOptional;

#define FSL_IMX8MM_SAI56_IRQ_LINE   (-2)

static const FslImx8mmOptional fsl_imx8mm_optional[] = {
    { TYPE_KICK_IMX8MM_SAI, "sai1", { FSL_IMX8MM_SAI1, -1 },
      { FSL_IMX8MM_SAI1_IRQ, -1 } },
    { TYPE_KICK_IMX8MM_SAI, "sai2", { FSL_IMX8MM_SAI2, -1 },
      { FSL_IMX8MM_SAI2_IRQ, -1 } },
    { TYPE_KICK_IMX8MM_SAI, "sai3", { FSL_IMX8MM_SAI3, -1 },
      { FSL_IMX8MM_SAI3_IRQ, -1 } },
    { TYPE_KICK_IMX8MM_SAI, "sai5", { FSL_IMX8MM_SAI5, -1 },
      { FSL_IMX8MM_SAI56_IRQ_LINE, -1 } },
    { TYPE_KICK_IMX8MM_SAI, "sai6", { FSL_IMX8MM_SAI6, -1 },
      { FSL_IMX8MM_SAI56_IRQ_LINE, -1 } },
    { TYPE_KICK_IMX8MM_SDMA, "sdma1", { FSL_IMX8MM_SDMA1, -1 },
      { FSL_IMX8MM_SDMA1_IRQ, -1 } },
    { TYPE_KICK_IMX8MM_SDMA, "sdma2", { FSL_IMX8MM_SDMA2, -1 },
      { FSL_IMX8MM_SDMA2_IRQ, -1 } },
    { TYPE_KICK_IMX8MM_SDMA, "sdma3", { FSL_IMX8MM_SDMA3, -1 },
      { FSL_IMX8MM_SDMA3_IRQ, -1 } },
};

DeviceState *fsl_imx8mm_get_optional(FslImx8mmState *s, const char *name)
{
    return s->optional ? g_hash_table_lookup(s->optional, name) : NULL;
}

static void fsl_imx8mm_init(Object *obj)
{
    FslImx8mmState *s = FSL_IMX8MM(obj);
    int i;

    object_initialize_child(obj, "gic", &s->gic, gicv3_class_name());
    object_initialize_child(obj, "anatop", &s->anatop, TYPE_IMX8MM_ANATOP);
    object_initialize_child(obj, "ccm", &s->ccm, TYPE_IMX8MM_CCM);
    object_initialize_child(obj, "src", &s->src, TYPE_IMX8MM_SRC);
    object_initialize_child(obj, "gpc", &s->gpc, TYPE_IMX8MM_GPC);
    object_initialize_child(obj, "iomuxc", &s->iomuxc, TYPE_IMX8MM_IOMUXC);
    object_initialize_child(obj, "iomuxc-gpr", &s->iomuxc_gpr,
                            TYPE_IMX8MM_IOMUXC_GPR);
    object_initialize_child(obj, "ocotp", &s->ocotp, TYPE_IMX8MM_OCOTP);
    object_initialize_child(obj, "snvs", &s->snvs, TYPE_IMX8MM_SNVS);
    object_initialize_child(obj, "tmu", &s->tmu, TYPE_IMX8MM_TMU);
    object_initialize_child(obj, "sysctr", &s->sysctr, TYPE_IMX8MM_SYSCTR);
    object_initialize_child(obj, "ddrc", &s->ddrc, TYPE_IMX8MM_DDRC);
    object_initialize_child(obj, "apbh-dma", &s->apbh_dma,
                            TYPE_IMX8MM_APBH_DMA);
    for (int i = 0; i < ARRAY_SIZE(s->pwm); i++) {
        g_autofree char *name = g_strdup_printf("pwm%d", i + 1);
        object_initialize_child(obj, name, &s->pwm[i], TYPE_IMX8MM_PWM);
    }

    for (i = 0; i < FSL_IMX8MM_NUM_UARTS; i++) {
        g_autofree char *name = g_strdup_printf("uart%d", i + 1);
        object_initialize_child(obj, name, &s->uart[i], TYPE_IMX8MM_UART);
    }
    for (i = 0; i < FSL_IMX8MM_NUM_GPTS; i++) {
        g_autofree char *name = g_strdup_printf("gpt%d", i + 1);
        g_autofree char *clk = g_strdup_printf("gpt%d-clock", i + 1);

        object_initialize_child(obj, clk, &s->gpt_clk[i],
                                TYPE_IMX8MM_GPT_CLOCK);
        object_initialize_child(obj, name, &s->gpt[i], TYPE_IMX7_GPT);
    }
    object_initialize_child(obj, "gpt5-gpt6-irq", &s->gpt5_gpt6_irq,
                            TYPE_OR_IRQ);
    for (i = 0; i < FSL_IMX8MM_NUM_I2CS; i++) {
        g_autofree char *name = g_strdup_printf("i2c%d", i + 1);
        object_initialize_child(obj, name, &s->i2c[i], TYPE_IMX8MM_I2C);
    }
    for (i = 0; i < FSL_IMX8MM_NUM_GPIOS; i++) {
        g_autofree char *name = g_strdup_printf("gpio%d", i + 1);
        object_initialize_child(obj, name, &s->gpio[i], TYPE_IMX_GPIO);
    }
    for (i = 0; i < FSL_IMX8MM_NUM_USDHCS; i++) {
        g_autofree char *name = g_strdup_printf("usdhc%d", i + 1);
        g_autofree char *cq = g_strdup_printf("usdhc%d-cqhci", i + 1);
        g_autofree char *irq = g_strdup_printf("usdhc%d-irq", i + 1);

        object_initialize_child(obj, name, &s->usdhc[i], TYPE_IMX8MM_USDHC);
        object_initialize_child(obj, cq, &s->cqhci[i], TYPE_IMX8MM_CQHCI);
        object_initialize_child(obj, irq, &s->usdhc_irq[i], TYPE_OR_IRQ);
    }
    for (i = 0; i < FSL_IMX8MM_NUM_ECSPIS; i++) {
        g_autofree char *name = g_strdup_printf("ecspi%d", i + 1);
        object_initialize_child(obj, name, &s->spi[i], TYPE_IMX_SPI);
    }
    for (i = 0; i < FSL_IMX8MM_NUM_WDTS; i++) {
        g_autofree char *name = g_strdup_printf("wdog%d", i + 1);
        object_initialize_child(obj, name, &s->wdt[i], TYPE_IMX2_WDT);
    }
    object_initialize_child(obj, "enet1", &s->enet, TYPE_IMX_ENET);
    for (i = 0; i < FSL_IMX8MM_NUM_USBS; i++) {
        g_autofree char *name = g_strdup_printf("usb%d", i + 1);

        object_initialize_child(obj, name, &s->usb[i], TYPE_IMX8MM_USB);
    }
    object_initialize_child(obj, "dispmix", &s->dispmix, TYPE_IMX8MM_DISPMIX);
    object_initialize_child(obj, "lcdif", &s->lcdif, TYPE_IMX8MM_LCDIF);
    object_initialize_child(obj, "mipi-dsim", &s->dsim, TYPE_IMX8MM_MIPI_DSIM);
    object_initialize_child(obj, "gpu3d", &s->gpu3d, TYPE_IMX8MM_GPU3D);
    object_initialize_child(obj, "gpu2d", &s->gpu2d, TYPE_IMX8MM_GPU2D);

    /* Optional peripherals that are compiled in */
    s->optional = g_hash_table_new(g_str_hash, g_str_equal);
    for (i = 0; i < ARRAY_SIZE(fsl_imx8mm_optional); i++) {
        const FslImx8mmOptional *o = &fsl_imx8mm_optional[i];
        Object *child;

        if (!object_class_by_name(o->type)) {
            continue;
        }
        child = object_new(o->type);
        object_property_add_child(obj, o->name, child);
        object_unref(child);
        g_hash_table_insert(s->optional, (gpointer)o->name, child);
    }
}

static void fsl_imx8mm_finalize(Object *obj)
{
    FslImx8mmState *s = FSL_IMX8MM(obj);

    g_hash_table_destroy(s->optional);
}

static bool fsl_imx8mm_realize_cpus(FslImx8mmState *s, Error **errp)
{
    for (int i = 0; i < s->num_cpus; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        Object *cpu = OBJECT(&s->cpu[i]);

        object_initialize_child(OBJECT(s), name, &s->cpu[i], s->cpu_type);

        /* Cortex-A53 CBAR: base of the GIC distributor */
        if (object_property_find(cpu, "reset-cbar")) {
            object_property_set_int(cpu, "reset-cbar", MMAP(GIC_DIST),
                                    &error_abort);
        }
        /* CNTFRQ: the system counter base frequency (24 MHz / 3) */
        if (object_property_find(cpu, "cntfrq")) {
            object_property_set_int(cpu, "cntfrq", s->cntfrq, &error_abort);
        }
        if (object_property_find(cpu, "has_el3")) {
            object_property_set_bool(cpu, "has_el3", s->has_el3,
                                     &error_abort);
        }
        if (object_property_find(cpu, "has_el2")) {
            object_property_set_bool(cpu, "has_el2", s->has_el2,
                                     &error_abort);
        }
        /* MPIDR: one cluster, Aff0 = core number */
        object_property_set_int(cpu, "mp-affinity", i, &error_abort);
        if (i) {
            /* Secondary cores are held off until PSCI or SRC releases them */
            object_property_set_bool(cpu, "start-powered-off", true,
                                     &error_abort);
        }
        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return false;
        }
    }
    return true;
}

static bool fsl_imx8mm_realize_gic(FslImx8mmState *s, Error **errp)
{
    DeviceState *gicdev = DEVICE(&s->gic);
    SysBusDevice *gicsbd = SYS_BUS_DEVICE(&s->gic);
    QList *redist_region_count;

    qdev_prop_set_uint32(gicdev, "num-cpu", s->num_cpus);
    qdev_prop_set_uint32(gicdev, "num-irq", FSL_IMX8MM_NUM_IRQS + GIC_INTERNAL);
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, s->num_cpus);
    qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);
    object_property_set_link(OBJECT(&s->gic), "sysmem",
                             OBJECT(get_system_memory()), &error_fatal);
    if (!sysbus_realize(gicsbd, errp)) {
        return false;
    }
    sysbus_mmio_map(gicsbd, 0, MMAP(GIC_DIST));
    sysbus_mmio_map(gicsbd, 1, MMAP(GIC_REDIST));

    /*
     * Generic timer, PMU and GIC maintenance outputs to the PPIs listed in
     * imx8mm.dtsi, GIC outputs to the CPU IRQ/FIQ/VIRQ/VFIQ inputs.
     */
    for (int i = 0; i < s->num_cpus; i++) {
        static const int timer_irqs[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
        };
        DeviceState *cpudev = DEVICE(&s->cpu[i]);
        int ppibase = FSL_IMX8MM_NUM_IRQS + i * GIC_INTERNAL;

        for (int j = 0; j < ARRAY_SIZE(timer_irqs); j++) {
            qdev_connect_gpio_out(cpudev, j,
                                  qdev_get_gpio_in(gicdev,
                                                   ppibase + timer_irqs[j]));
        }
        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gicdev, ppibase +
                                                     ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gicdev, ppibase +
                                                     VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicsbd, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicsbd, i + s->num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicsbd, i + 2 * s->num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicsbd, i + 3 * s->num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
    return true;
}

static qemu_irq fsl_imx8mm_irq(FslImx8mmState *s, int spi)
{
    return qdev_get_gpio_in(DEVICE(&s->gic), spi);
}

/* Realize a sysbus child, map MMIO 0 at @addr and connect IRQ 0 to @spi */
static bool fsl_imx8mm_sysbus(FslImx8mmState *s, void *dev, hwaddr addr,
                              int spi, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, addr);
    if (spi >= 0) {
        sysbus_connect_irq(sbd, 0, fsl_imx8mm_irq(s, spi));
    }
    return true;
}

static bool fsl_imx8mm_realize_clocks(FslImx8mmState *s, Error **errp)
{
    if (!fsl_imx8mm_sysbus(s, &s->anatop, MMAP(ANATOP), -1, errp)) {
        return false;
    }
    object_property_set_link(OBJECT(&s->ccm), "anatop", OBJECT(&s->anatop),
                             &error_abort);
    if (!fsl_imx8mm_sysbus(s, &s->ccm, MMAP(CCM), -1, errp)) {
        return false;
    }
    for (int i = 0; i < FSL_IMX8MM_NUM_GPTS; i++) {
        object_property_set_link(OBJECT(&s->gpt_clk[i]), "ccm",
                                 OBJECT(&s->ccm), &error_abort);
        qdev_prop_set_uint32(DEVICE(&s->gpt_clk[i]), "root-offset",
                             IMX8MM_CCM_ROOT_GPT1 + i * 0x80);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpt_clk[i]), errp)) {
            return false;
        }
    }
    return true;
}

static bool fsl_imx8mm_realize_system(FslImx8mmState *s, Error **errp)
{
    qdev_prop_set_uint32(DEVICE(&s->src), "num-cpus", s->num_cpus);
    if (!fsl_imx8mm_sysbus(s, &s->src, MMAP(SRC), FSL_IMX8MM_SRC_IRQ, errp) ||
        !fsl_imx8mm_sysbus(s, &s->gpc, MMAP(GPC), FSL_IMX8MM_GPC_IRQ, errp) ||
        !fsl_imx8mm_sysbus(s, &s->iomuxc, MMAP(IOMUXC), -1, errp) ||
        !fsl_imx8mm_sysbus(s, &s->iomuxc_gpr, MMAP(IOMUXC_GPR), -1, errp) ||
        !fsl_imx8mm_sysbus(s, &s->ocotp, MMAP(OCOTP_CTRL), -1, errp) ||
        /*
         * TMU: SPI 49 (TEMPMON_LOW_IRQn in NXP's MIMX8MM6_cm4.h). Not listed
         * in imx8mm.dtsi, so Linux does not use it.
         */
        !fsl_imx8mm_sysbus(s, &s->tmu, MMAP(TMU), FSL_IMX8MM_TMU_IRQ, errp) ||
        !fsl_imx8mm_sysbus(s, &s->snvs, MMAP(SNVS_HP),
                           FSL_IMX8MM_SNVS_RTC_IRQ, errp)) {
        return false;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->snvs), 1,
                       fsl_imx8mm_irq(s, FSL_IMX8MM_SNVS_PWRKEY_IRQ));

    /* PWM1..4: PWM1 is the panel backlight on the KICK (pwm-backlight) */
    for (int i = 0; i < ARRAY_SIZE(s->pwm); i++) {
        if (!fsl_imx8mm_sysbus(s, &s->pwm[i], MMAP(PWM1) + i * 0x10000,
                               FSL_IMX8MM_PWM1_IRQ + i, errp)) {
            return false;
        }
    }

    /* APBH DMA: registers for the mxs-dma probe, no channel ever runs */
    if (!fsl_imx8mm_sysbus(s, &s->apbh_dma, MMAP(APBH_DMA), -1, errp)) {
        return false;
    }

    /* DDR controller (MMIO 0) and DDR PHY (MMIO 1) */
    if (!fsl_imx8mm_sysbus(s, &s->ddrc, MMAP(DDR_CTL), -1, errp)) {
        return false;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ddrc), 1, MMAP(DDR_PHY));

    /* System counter: read, compare and control frames */
    if (!fsl_imx8mm_sysbus(s, &s->sysctr, MMAP(SYSCNT_RD),
                           FSL_IMX8MM_SYSCNT_IRQ, errp)) {
        return false;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctr), 1, MMAP(SYSCNT_CMP));
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctr), 2, MMAP(SYSCNT_CTRL));
    /* Compare frame 1 uses the next SPI, as on the i.MX8MQ */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sysctr), 1,
                       fsl_imx8mm_irq(s, FSL_IMX8MM_SYSCNT_IRQ + 1));
    return true;
}

static bool fsl_imx8mm_realize_peripherals(FslImx8mmState *s, Error **errp)
{
    static const int uart_mmap[] = {
        FSL_IMX8MM_UART1, FSL_IMX8MM_UART2, FSL_IMX8MM_UART3, FSL_IMX8MM_UART4
    };
    static const int uart_irq[] = {
        FSL_IMX8MM_UART1_IRQ, FSL_IMX8MM_UART2_IRQ,
        FSL_IMX8MM_UART3_IRQ, FSL_IMX8MM_UART4_IRQ
    };
    static const int gpt_mmap[] = {
        FSL_IMX8MM_GPT1, FSL_IMX8MM_GPT2, FSL_IMX8MM_GPT3,
        FSL_IMX8MM_GPT4, FSL_IMX8MM_GPT5, FSL_IMX8MM_GPT6
    };
    static const int gpt_irq[] = {
        FSL_IMX8MM_GPT1_IRQ, FSL_IMX8MM_GPT2_IRQ,
        FSL_IMX8MM_GPT3_IRQ, FSL_IMX8MM_GPT4_IRQ
    };
    static const int i2c_irq[] = {
        FSL_IMX8MM_I2C1_IRQ, FSL_IMX8MM_I2C2_IRQ,
        FSL_IMX8MM_I2C3_IRQ, FSL_IMX8MM_I2C4_IRQ
    };
    static const int spi_irq[] = {
        FSL_IMX8MM_ECSPI1_IRQ, FSL_IMX8MM_ECSPI2_IRQ, FSL_IMX8MM_ECSPI3_IRQ
    };
    static const int usdhc_irq[] = {
        FSL_IMX8MM_USDHC1_IRQ, FSL_IMX8MM_USDHC2_IRQ, FSL_IMX8MM_USDHC3_IRQ
    };
    static const int wdog_irq[] = {
        FSL_IMX8MM_WDOG1_IRQ, FSL_IMX8MM_WDOG2_IRQ, FSL_IMX8MM_WDOG3_IRQ
    };
    int i;

    /*
     * UART1..4 on serial_hd(0..3). The UART raises its interrupt through
     * the named "irq" output; its line timing follows the UARTn clock root.
     */
    for (i = 0; i < FSL_IMX8MM_NUM_UARTS; i++) {
        qdev_prop_set_chr(DEVICE(&s->uart[i]), "chardev", serial_hd(i));
        object_property_set_link(OBJECT(&s->uart[i]), "ccm", OBJECT(&s->ccm),
                                 &error_abort);
        qdev_prop_set_uint32(DEVICE(&s->uart[i]), "ccm-root",
                             IMX8MM_CCM_ROOT_UART1 + i * 0x80);
        if (!fsl_imx8mm_sysbus(s, &s->uart[i],
                               fsl_imx8mm_memmap[uart_mmap[i]].addr,
                               -1, errp)) {
            return false;
        }
        qdev_connect_gpio_out_named(DEVICE(&s->uart[i]), IMX8MM_UART_GPIO_IRQ,
                                    0, fsl_imx8mm_irq(s, uart_irq[i]));
    }

    /* GPT1..4 have their own SPI, GPT5 and GPT6 share one */
    qdev_prop_set_uint16(DEVICE(&s->gpt5_gpt6_irq), "num-lines", 2);
    if (!qdev_realize(DEVICE(&s->gpt5_gpt6_irq), NULL, errp)) {
        return false;
    }
    qdev_connect_gpio_out(DEVICE(&s->gpt5_gpt6_irq), 0,
                          fsl_imx8mm_irq(s, FSL_IMX8MM_GPT5_GPT6_IRQ));
    for (i = 0; i < FSL_IMX8MM_NUM_GPTS; i++) {
        s->gpt[i].ccm = IMX_CCM(&s->gpt_clk[i]);
        if (!fsl_imx8mm_sysbus(s, &s->gpt[i],
                               fsl_imx8mm_memmap[gpt_mmap[i]].addr,
                               i < 4 ? gpt_irq[i] : -1, errp)) {
            return false;
        }
        if (i >= 4) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpt[i]), 0,
                               qdev_get_gpio_in(DEVICE(&s->gpt5_gpt6_irq),
                                                i - 4));
        }
    }

    /* I2C1..4: the 8MM wrapper's own window carries the full register set */
    for (i = 0; i < FSL_IMX8MM_NUM_I2CS; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->i2c[i]);

        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        sysbus_mmio_map(sbd, IMX8MM_I2C_MMIO, MMAP(I2C1) + i * 0x10000);
        sysbus_connect_irq(sbd, 0, fsl_imx8mm_irq(s, i2c_irq[i]));
    }

    /* GPIO1..5: combined interrupts for pins 0-15 and 16-31 */
    for (i = 0; i < FSL_IMX8MM_NUM_GPIOS; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->gpio[i]);

        object_property_set_bool(OBJECT(sbd), "has-edge-sel", true,
                                 &error_abort);
        object_property_set_bool(OBJECT(sbd), "has-upper-pin-irq", true,
                                 &error_abort);
        if (!fsl_imx8mm_sysbus(s, sbd, MMAP(GPIO1) + i * 0x10000,
                               FSL_IMX8MM_GPIO1_LOW_IRQ + 2 * i, errp)) {
            return false;
        }
        sysbus_connect_irq(sbd, 1,
                           fsl_imx8mm_irq(s, FSL_IMX8MM_GPIO1_HIGH_IRQ + 2 * i));
    }
    /*
     * GPIO1 pins 0..7 also have their own interrupt: INT0 is SPI 63 down to
     * INT7 at SPI 56 (GPIO1_INTn_IRQn in NXP's MIMX8MM6_cm4.h).
     */
    for (i = 0; i < IMX_GPIO_PIN_IRQ_COUNT; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->gpio[0]), "pin-irq", i,
                                    fsl_imx8mm_irq(s,
                                        FSL_IMX8MM_GPIO1_INT7_IRQ + 7 - i));
    }

    /*
     * uSDHC1..3. The SDHCI core registers end at 0x100; the command queue
     * engine follows at +0x100 and shares the controller interrupt.
     */
    for (i = 0; i < FSL_IMX8MM_NUM_USDHCS; i++) {
        hwaddr base = MMAP(USDHC1) + i * 0x10000;
        DeviceState *orirq = DEVICE(&s->usdhc_irq[i]);

        qdev_prop_set_uint16(orirq, "num-lines", 2);
        if (!qdev_realize(orirq, NULL, errp)) {
            return false;
        }
        qdev_connect_gpio_out(orirq, 0, fsl_imx8mm_irq(s, usdhc_irq[i]));
        if (!fsl_imx8mm_sysbus(s, &s->usdhc[i], base, -1, errp) ||
            !fsl_imx8mm_sysbus(s, &s->cqhci[i], base + IMX8MM_CQHCI_OFFSET,
                               -1, errp)) {
            return false;
        }
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->usdhc[i]), 0,
                           qdev_get_gpio_in(orirq, 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->cqhci[i]), 0,
                           qdev_get_gpio_in(orirq, 1));
    }

    for (i = 0; i < FSL_IMX8MM_NUM_ECSPIS; i++) {
        if (!fsl_imx8mm_sysbus(s, &s->spi[i], MMAP(ECSPI1) + i * 0x10000,
                               spi_irq[i], errp)) {
            return false;
        }
    }

    for (i = 0; i < FSL_IMX8MM_NUM_WDTS; i++) {
        object_property_set_bool(OBJECT(&s->wdt[i]), "pretimeout-support",
                                 true, &error_abort);
        if (!fsl_imx8mm_sysbus(s, &s->wdt[i], MMAP(WDOG1) + i * 0x10000,
                               wdog_irq[i], errp)) {
            return false;
        }
    }

    /*
     * ENET1 with three TX queues. The MAC's "other events" interrupt is
     * SPI 120 (queue 0), the IEEE 1588 timer SPI 121. When the board
     * attaches an external PHY model it disables the built-in one.
     */
    object_property_set_bool(OBJECT(&s->enet), "phy-connected",
                             s->enet_phy_connected, &error_abort);
    object_property_set_uint(OBJECT(&s->enet), "tx-ring-num", 3,
                             &error_abort);
    qemu_configure_nic_device(DEVICE(&s->enet), true, NULL);
    if (!fsl_imx8mm_sysbus(s, &s->enet, MMAP(ENET1),
                           FSL_IMX8MM_ENET1_MAC_IRQ, errp)) {
        return false;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->enet), 1,
                       fsl_imx8mm_irq(s, FSL_IMX8MM_ENET1_1588_IRQ));

    /*
     * USB1 (OTG, the KICK's USB-C port) and USB2 (host, USB-A) in host
     * role. The device ids name their USB buses "usb-c.0" and "usb-a.0"
     * (docs/host-interface.md), so devices attach with bus=usb-a.0.
     */
    for (i = 0; i < FSL_IMX8MM_NUM_USBS; i++) {
        static const char *const bus_id[FSL_IMX8MM_NUM_USBS] = {
            "usb-c", "usb-a"
        };
        hwaddr base = MMAP(USB1) + i * 0x10000;
        DeviceState *dev = DEVICE(&s->usb[i]);

        g_free(dev->id);
        dev->id = g_strdup(bus_id[i]);
        if (!fsl_imx8mm_sysbus(s, &s->usb[i], base,
                               FSL_IMX8MM_USB1_IRQ + i, errp)) {
            return false;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1,
                        base + IMX8MM_USBMISC_OFFSET);
    }
    return true;
}

/*
 * Display mix (block control), LCDIF, MIPI DSIM and the Vivante GC7000UL /
 * GC520L cores. The LCDIF and the GPU cores master the system bus. The
 * DSIM's MIPI_DSI_I_PRESET input follows DISPMIX soft reset bit 5, so
 * software has to release it as the vendor reset driver does.
 */
static bool fsl_imx8mm_realize_display(FslImx8mmState *s, Error **errp)
{
    Object *sysmem = OBJECT(get_system_memory());

    object_property_set_link(OBJECT(&s->lcdif), "dma-mr", sysmem,
                             &error_abort);
    object_property_set_link(OBJECT(&s->gpu3d), "dma-mr", sysmem,
                             &error_abort);
    object_property_set_link(OBJECT(&s->gpu2d), "dma-mr", sysmem,
                             &error_abort);

    if (!fsl_imx8mm_sysbus(s, &s->dispmix, MMAP(DISP_BLK_CTRL), -1, errp) ||
        !fsl_imx8mm_sysbus(s, &s->lcdif, MMAP(LCDIF), FSL_IMX8MM_LCDIF_IRQ,
                           errp) ||
        !fsl_imx8mm_sysbus(s, &s->dsim, MMAP(MIPI_DSI),
                           FSL_IMX8MM_MIPI_DSI_IRQ, errp) ||
        !fsl_imx8mm_sysbus(s, &s->gpu3d, MMAP(GPU3D), FSL_IMX8MM_GPU3D_IRQ,
                           errp) ||
        !fsl_imx8mm_sysbus(s, &s->gpu2d, MMAP(GPU2D), FSL_IMX8MM_GPU2D_IRQ,
                           errp)) {
        return false;
    }
    qdev_connect_gpio_out_named(DEVICE(&s->dispmix), "sft-rstn",
                                FSL_IMX8MM_DISPMIX_RST_DSI_PRESET,
                                qdev_get_gpio_in_named(DEVICE(&s->dsim),
                                                       "reset-n", 0));
    return true;
}

static bool fsl_imx8mm_realize_optional(FslImx8mmState *s, Error **errp)
{
    DeviceState *sai56_or = NULL;

    for (int i = 0; i < ARRAY_SIZE(fsl_imx8mm_optional); i++) {
        const FslImx8mmOptional *o = &fsl_imx8mm_optional[i];
        DeviceState *dev = fsl_imx8mm_get_optional(s, o->name);
        SysBusDevice *sbd;

        if (!dev) {
            continue;
        }
        sbd = SYS_BUS_DEVICE(dev);
        /*
         * The SAI models take their master clock as a property; MCLK1 is
         * the SAIn clock root, which the DT assigns to AUDIO_PLL1 at
         * 12.288 MHz (assigned-clock-rates in the sai nodes).
         */
        if (!strcmp(o->type, TYPE_KICK_IMX8MM_SAI)) {
            qdev_prop_set_uint32(dev, "mclk1-hz", s->sai_mclk_hz);
        }
        if (!strcmp(o->type, TYPE_KICK_IMX8MM_SDMA)) {
            if (s->sdma_firmware) {
                qdev_prop_set_string(dev, "firmware", s->sdma_firmware);
            }
            qdev_prop_set_bit(dev, "infer-scripts", s->sdma_infer_scripts);
        }
        /* Bus masters get the system address space for their DMA */
        if (object_property_find(OBJECT(dev), "dma-mr")) {
            object_property_set_link(OBJECT(dev), "dma-mr",
                                     OBJECT(get_system_memory()),
                                     &error_abort);
        }
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        for (int m = 0; m < 2; m++) {
            if (o->mmio[m] >= 0 && sysbus_has_mmio(sbd, m)) {
                sysbus_mmio_map(sbd, m, fsl_imx8mm_memmap[o->mmio[m]].addr);
            }
        }
        for (int n = 0; n < 2; n++) {
            qemu_irq irq;

            if (o->irq[n] == -1 || !sysbus_has_irq(sbd, n)) {
                continue;
            }
            if (o->irq[n] == FSL_IMX8MM_SAI56_IRQ_LINE) {
                if (!sai56_or) {
                    sai56_or = qdev_new(TYPE_OR_IRQ);
                    object_property_add_child(OBJECT(s), "sai5-sai6-irq",
                                              OBJECT(sai56_or));
                    qdev_prop_set_uint16(sai56_or, "num-lines", 2);
                    qdev_realize_and_unref(sai56_or, NULL, &error_abort);
                    qdev_connect_gpio_out(sai56_or, 0,
                        fsl_imx8mm_irq(s, FSL_IMX8MM_SAI5_SAI6_IRQ));
                }
                irq = qdev_get_gpio_in(sai56_or,
                                       !strcmp(o->name, "sai6") ? 1 : 0);
            } else {
                irq = fsl_imx8mm_irq(s, o->irq[n]);
            }
            sysbus_connect_irq(sbd, n, irq);
        }
    }
    return true;
}

/*
 * SDMA event inputs (imx8mm.dtsi "dmas"): the UARTs request SDMA1, the
 * SAIs SDMA2. Wired only when the SDMA model is built in.
 */
static void fsl_imx8mm_wire_dma(FslImx8mmState *s)
{
    static const struct {
        const char *dev;    /* NULL: one of the UARTs (index in uart) */
        int uart;
        const char *sdma;
        int rx_event;
        int tx_event;
    } map[] = {
        { NULL, 0, "sdma1", 22, 23 },   /* UART1 */
        { NULL, 2, "sdma1", 26, 27 },   /* UART3 */
        { NULL, 3, "sdma1", 28, 29 },   /* UART4 */
        { "sai1", 0, "sdma2", 0, 1 },
        { "sai2", 0, "sdma2", 2, 3 },
        { "sai3", 0, "sdma2", 4, 5 },
        { "sai5", 0, "sdma2", 8, 9 },
        { "sai6", 0, "sdma2", 10, 11 },
    };

    for (int i = 0; i < ARRAY_SIZE(map); i++) {
        DeviceState *sdma = fsl_imx8mm_get_optional(s, map[i].sdma);
        DeviceState *dev = map[i].dev ?
            fsl_imx8mm_get_optional(s, map[i].dev) :
            DEVICE(&s->uart[map[i].uart]);

        if (!sdma || !dev) {
            continue;
        }
        qdev_connect_gpio_out_named(dev, "dma-rx", 0,
                                    qdev_get_gpio_in_named(sdma, "events",
                                                           map[i].rx_event));
        qdev_connect_gpio_out_named(dev, "dma-tx", 0,
                                    qdev_get_gpio_in_named(sdma, "events",
                                                           map[i].tx_event));
    }
}

static bool fsl_imx8mm_add_ram(FslImx8mmState *s, MemoryRegion *mr, int id,
                               const char *name, Error **errp)
{
    if (!memory_region_init_ram(mr, OBJECT(s), name,
                                fsl_imx8mm_memmap[id].size, errp)) {
        return false;
    }
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx8mm_memmap[id].addr, mr);
    return true;
}

static void fsl_imx8mm_realize(DeviceState *dev, Error **errp)
{
    FslImx8mmState *s = FSL_IMX8MM(dev);

    if (!s->cpu_type) {
        error_setg(errp, "%s: 'cpu-type' not set", TYPE_FSL_IMX8MM);
        return;
    }
    if (s->num_cpus < 1 || s->num_cpus > FSL_IMX8MM_NUM_CPUS) {
        error_setg(errp, "%s: %u CPUs requested, the i.MX8MM has 1..%d",
                   TYPE_FSL_IMX8MM, s->num_cpus, FSL_IMX8MM_NUM_CPUS);
        return;
    }

    if (!fsl_imx8mm_realize_cpus(s, errp) ||
        !fsl_imx8mm_realize_gic(s, errp) ||
        !fsl_imx8mm_realize_clocks(s, errp) ||
        !fsl_imx8mm_realize_system(s, errp) ||
        !fsl_imx8mm_realize_peripherals(s, errp) ||
        !fsl_imx8mm_realize_display(s, errp) ||
        !fsl_imx8mm_realize_optional(s, errp)) {
        return;
    }
    fsl_imx8mm_wire_dma(s);

    /*
     * On-chip memories. The boot ROM window is a ROM region the BootROM
     * emulation (boot=rom) fills; OCRAM, OCRAM_S, the M4 TCMs and the CAAM
     * secure RAM are plain RAM.
     */
    /*
     * One ROM region spans the ROM and its protected last 4 KiB, so the
     * mapping stays 16 KiB aligned (HVF maps memory in host pages).
     */
    if (!memory_region_init_rom(&s->boot_rom, OBJECT(s), "imx8mm.boot-rom",
                                fsl_imx8mm_memmap[FSL_IMX8MM_BOOT_ROM].size +
                                fsl_imx8mm_memmap[FSL_IMX8MM_BOOT_ROM_PROTECTED].size,
                                errp)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), MMAP(BOOT_ROM),
                                &s->boot_rom);
    if (!fsl_imx8mm_add_ram(s, &s->ocram, FSL_IMX8MM_OCRAM, "imx8mm.ocram",
                            errp) ||
        !fsl_imx8mm_add_ram(s, &s->ocram_s, FSL_IMX8MM_OCRAM_S,
                            "imx8mm.ocram-s", errp) ||
        !fsl_imx8mm_add_ram(s, &s->tcm_itcm, FSL_IMX8MM_TCM_ITCM,
                            "imx8mm.itcm", errp) ||
        !fsl_imx8mm_add_ram(s, &s->tcm_dtcm, FSL_IMX8MM_TCM_DTCM,
                            "imx8mm.dtcm", errp) ||
        !fsl_imx8mm_add_ram(s, &s->caam_mem, FSL_IMX8MM_CAAM_MEM,
                            "imx8mm.caam-mem", errp)) {
        return;
    }

    /*
     * Every peripheral window gets an unimplemented-device at the lowest
     * priority. Where a model exists it sits on top; accesses to blocks
     * without a model, or to the part of a 64 KiB slot a model does not
     * decode, are logged with "-d unimp" instead of aborting.
     */
    for (int i = 0; i < FSL_IMX8MM_NUM_MEMORY_REGIONS; i++) {
        switch (i) {
        case FSL_IMX8MM_BOOT_ROM:
        case FSL_IMX8MM_BOOT_ROM_PROTECTED:
        case FSL_IMX8MM_CAAM_MEM:
        case FSL_IMX8MM_OCRAM_S:
        case FSL_IMX8MM_TCM_ITCM:
        case FSL_IMX8MM_TCM_DTCM:
        case FSL_IMX8MM_OCRAM:
        case FSL_IMX8MM_GIC_DIST:
        case FSL_IMX8MM_GIC_REDIST:
        case FSL_IMX8MM_RAM:
            /* memories and the GIC */
            break;
        default:
            create_unimplemented_device(fsl_imx8mm_memmap[i].name,
                                        fsl_imx8mm_memmap[i].addr,
                                        fsl_imx8mm_memmap[i].size);
            break;
        }
    }
}

static const Property fsl_imx8mm_properties[] = {
    DEFINE_PROP_STRING("cpu-type", FslImx8mmState, cpu_type),
    DEFINE_PROP_UINT32("num-cpus", FslImx8mmState, num_cpus,
                       FSL_IMX8MM_NUM_CPUS),
    DEFINE_PROP_UINT64("cntfrq", FslImx8mmState, cntfrq, 8000000),
    DEFINE_PROP_BOOL("has-el3", FslImx8mmState, has_el3, false),
    DEFINE_PROP_BOOL("has-el2", FslImx8mmState, has_el2, false),
    DEFINE_PROP_BOOL("enet-phy-connected", FslImx8mmState,
                     enet_phy_connected, true),
    /* SAIn MCLK1 (SAIn clock root) as assigned by the board DT */
    DEFINE_PROP_UINT32("sai-mclk-hz", FslImx8mmState, sai_mclk_hz, 12288000),
    /* SDMA RAM firmware the guest loads, for its script entry points */
    DEFINE_PROP_STRING("sdma-firmware", FslImx8mmState, sdma_firmware),
    DEFINE_PROP_BOOL("sdma-infer-scripts", FslImx8mmState, sdma_infer_scripts,
                     true),
};

static void fsl_imx8mm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, fsl_imx8mm_properties);
    dc->realize = fsl_imx8mm_realize;
    dc->desc = "NXP i.MX 8M Mini SoC";
    /* Created by the board only, it wires up serial_hd() and the NIC */
    dc->user_creatable = false;
}

static const TypeInfo fsl_imx8mm_types[] = {
    {
        .name = TYPE_FSL_IMX8MM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(FslImx8mmState),
        .instance_init = fsl_imx8mm_init,
        .instance_finalize = fsl_imx8mm_finalize,
        .class_init = fsl_imx8mm_class_init,
    },
};

DEFINE_TYPES(fsl_imx8mm_types)
