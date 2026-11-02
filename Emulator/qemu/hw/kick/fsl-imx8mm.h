/*
 * NXP i.MX 8M Mini SoC
 *
 * Memory map and interrupt numbers follow the i.MX 8M Mini Applications
 * Processor Reference Manual (IMX8MMRM), chapter 2 "Memory Maps" and
 * chapter 3 "Interrupts and DMA Events", cross-checked against
 * arch/arm64/boot/dts/freescale/imx8mm.dtsi.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_FSL_IMX8MM_H
#define KICK_FSL_IMX8MM_H

#include "cpu.h"
#include "hw/char/imx_serial.h"
#include "hw/core/or-irq.h"
#include "hw/core/sysbus.h"
#include "hw/gpio/imx_gpio.h"
#include "hw/i2c/imx_i2c.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/net/imx_fec.h"
#include "hw/sd/sdhci.h"
#include "hw/ssi/imx_spi.h"
#include "hw/timer/imx_gpt.h"
#include "hw/watchdog/wdt_imx2.h"
#include "qemu/units.h"
#include "qom/object.h"

#include "imx8mm_anatop.h"
#include "imx8mm_apbh_dma.h"
#include "imx8mm_ccm.h"
#include "imx8mm_cqhci.h"
#include "imx8mm_ddrc.h"
#include "imx8mm_dispmix.h"
#include "imx8mm_gpc.h"
#include "imx8mm_i2c.h"
#include "imx8mm_iomuxc.h"
#include "imx8mm_lcdif.h"
#include "imx8mm_mipi_dsim.h"
#include "imx8mm_ocotp.h"
#include "imx8mm_snvs.h"
#include "imx8mm_src.h"
#include "imx8mm_sysctr.h"
#include "imx8mm_pwm.h"
#include "imx8mm_tmu.h"
#include "imx8mm_uart.h"
#include "imx8mm_usb.h"
#include "imx8mm_usdhc.h"
#include "vivante_gpu.h"

#define TYPE_FSL_IMX8MM "fsl-imx8mm"
OBJECT_DECLARE_SIMPLE_TYPE(FslImx8mmState, FSL_IMX8MM)

/* DDR window: 0x4000_0000 up to 0x1_3FFF_FFFF (RM table 2-1, 4 GiB max) */
#define FSL_IMX8MM_RAM_START        0x40000000ULL
#define FSL_IMX8MM_RAM_SIZE_MAX     (4 * GiB)

/* OCRAM: SPL is loaded here by the BootROM, ATF BL31 runs from 0x920000 */
#define FSL_IMX8MM_OCRAM_START      0x00900000
#define FSL_IMX8MM_OCRAM_SIZE       (256 * KiB)

enum FslImx8mmConfiguration {
    FSL_IMX8MM_NUM_CPUS         = 4,
    FSL_IMX8MM_NUM_ECSPIS       = 3,
    FSL_IMX8MM_NUM_GPIOS        = 5,
    FSL_IMX8MM_NUM_GPTS         = 6,
    FSL_IMX8MM_NUM_I2CS         = 4,
    /* 128 shared peripheral interrupts (RM table 3-1) */
    FSL_IMX8MM_NUM_IRQS         = 128,
    FSL_IMX8MM_NUM_UARTS        = 4,
    FSL_IMX8MM_NUM_USBS         = 2,
    FSL_IMX8MM_NUM_USDHCS       = 3,
    FSL_IMX8MM_NUM_WDTS         = 3,
};

/*
 * Memory map. Entries that have a model are instantiated explicitly in
 * fsl-imx8mm.c, everything else is mapped as an unimplemented-device so that
 * guest accesses show up with -d unimp.
 */
enum FslImx8mmMemoryRegions {
    FSL_IMX8MM_BOOT_ROM,
    FSL_IMX8MM_BOOT_ROM_PROTECTED,
    FSL_IMX8MM_CAAM_MEM,
    FSL_IMX8MM_OCRAM_S,
    FSL_IMX8MM_TCM_ITCM,
    FSL_IMX8MM_TCM_DTCM,
    FSL_IMX8MM_OCRAM,
    FSL_IMX8MM_QSPI_MEM,
    FSL_IMX8MM_PCIE1_MEM,
    FSL_IMX8MM_A53_DAP,

    /* AIPS-1 */
    FSL_IMX8MM_SAI1,
    FSL_IMX8MM_SAI2,
    FSL_IMX8MM_SAI3,
    FSL_IMX8MM_SAI5,
    FSL_IMX8MM_SAI6,
    FSL_IMX8MM_MICFIL,
    FSL_IMX8MM_SPDIF1,
    FSL_IMX8MM_SPBA2,
    FSL_IMX8MM_AIPS1_CONFIGURATION,
    FSL_IMX8MM_GPIO1,
    FSL_IMX8MM_GPIO2,
    FSL_IMX8MM_GPIO3,
    FSL_IMX8MM_GPIO4,
    FSL_IMX8MM_GPIO5,
    FSL_IMX8MM_TMU,
    FSL_IMX8MM_ANA_OSC,
    FSL_IMX8MM_WDOG1,
    FSL_IMX8MM_WDOG2,
    FSL_IMX8MM_WDOG3,
    FSL_IMX8MM_SDMA3,
    FSL_IMX8MM_SDMA2,
    FSL_IMX8MM_GPT1,
    FSL_IMX8MM_GPT2,
    FSL_IMX8MM_GPT3,
    FSL_IMX8MM_ROMCP,
    FSL_IMX8MM_IOMUXC,
    FSL_IMX8MM_IOMUXC_GPR,
    FSL_IMX8MM_OCOTP_CTRL,
    FSL_IMX8MM_ANATOP,
    FSL_IMX8MM_SNVS_HP,
    FSL_IMX8MM_CCM,
    FSL_IMX8MM_SRC,
    FSL_IMX8MM_GPC,
    FSL_IMX8MM_SEMAPHORE1,
    FSL_IMX8MM_SEMAPHORE2,
    FSL_IMX8MM_RDC,
    FSL_IMX8MM_CSU,

    /* AIPS-2 */
    FSL_IMX8MM_AIPS2_CONFIGURATION,
    FSL_IMX8MM_PWM1,
    FSL_IMX8MM_PWM2,
    FSL_IMX8MM_PWM3,
    FSL_IMX8MM_PWM4,
    FSL_IMX8MM_SYSCNT_RD,
    FSL_IMX8MM_SYSCNT_CMP,
    FSL_IMX8MM_SYSCNT_CTRL,
    FSL_IMX8MM_GPT6,
    FSL_IMX8MM_GPT5,
    FSL_IMX8MM_GPT4,
    FSL_IMX8MM_PERFMON1,
    FSL_IMX8MM_PERFMON2,
    FSL_IMX8MM_QOSC,

    /* AIPS-3 */
    FSL_IMX8MM_ECSPI1,
    FSL_IMX8MM_ECSPI2,
    FSL_IMX8MM_ECSPI3,
    FSL_IMX8MM_UART1,
    FSL_IMX8MM_UART3,
    FSL_IMX8MM_UART2,
    FSL_IMX8MM_SPBA1,
    FSL_IMX8MM_CAAM,
    FSL_IMX8MM_AIPS3_CONFIGURATION,
    FSL_IMX8MM_I2C1,
    FSL_IMX8MM_I2C2,
    FSL_IMX8MM_I2C3,
    FSL_IMX8MM_I2C4,
    FSL_IMX8MM_UART4,
    FSL_IMX8MM_MU_A,
    FSL_IMX8MM_MU_B,
    FSL_IMX8MM_SEMAPHORE_HS,
    FSL_IMX8MM_USDHC1,
    FSL_IMX8MM_USDHC2,
    FSL_IMX8MM_USDHC3,
    FSL_IMX8MM_QSPI,
    FSL_IMX8MM_SDMA1,
    FSL_IMX8MM_ENET1,

    /* NOC and AIPS-4 */
    FSL_IMX8MM_NOC,
    FSL_IMX8MM_AIPS4_CONFIGURATION,
    FSL_IMX8MM_LCDIF,
    FSL_IMX8MM_MIPI_DSI,
    FSL_IMX8MM_CSI1,
    FSL_IMX8MM_DISP_BLK_CTRL,
    FSL_IMX8MM_MIPI_CSI,
    FSL_IMX8MM_USB1,
    FSL_IMX8MM_USB2,
    FSL_IMX8MM_PCIE_PHY1,
    FSL_IMX8MM_TZASC,

    FSL_IMX8MM_APBH_DMA,
    FSL_IMX8MM_GPMI,
    FSL_IMX8MM_PCIE1,
    FSL_IMX8MM_GPU3D,
    FSL_IMX8MM_GPU2D,
    FSL_IMX8MM_VPU_G1,
    FSL_IMX8MM_VPU_G2,
    FSL_IMX8MM_VPU_H1,
    FSL_IMX8MM_VPU_BLK_CTRL,
    FSL_IMX8MM_GIC_DIST,
    FSL_IMX8MM_GIC_REDIST,
    FSL_IMX8MM_DDR_PHY,
    FSL_IMX8MM_DDR_BLK_CTRL,
    FSL_IMX8MM_DDR_CTL,
    FSL_IMX8MM_DDR_PERF_MON,
    FSL_IMX8MM_RAM,

    FSL_IMX8MM_NUM_MEMORY_REGIONS
};

typedef struct FslImx8mmMemMapEntry {
    hwaddr addr;
    hwaddr size;
    const char *name;
} FslImx8mmMemMapEntry;

extern const FslImx8mmMemMapEntry fsl_imx8mm_memmap[FSL_IMX8MM_NUM_MEMORY_REGIONS];

/* Shared peripheral interrupt numbers (GIC SPI, RM table 3-1) */
enum FslImx8mmIrqs {
    FSL_IMX8MM_SDMA1_IRQ        = 2,
    FSL_IMX8MM_GPU3D_IRQ        = 3,
    FSL_IMX8MM_SNVS_PWRKEY_IRQ  = 4,
    FSL_IMX8MM_LCDIF_IRQ        = 5,
    FSL_IMX8MM_SPDIF1_IRQ       = 6,
    FSL_IMX8MM_VPU_G1_IRQ       = 7,
    FSL_IMX8MM_VPU_G2_IRQ       = 8,
    FSL_IMX8MM_WDOG3_IRQ        = 10,
    FSL_IMX8MM_APBH_DMA_IRQ     = 12,
    FSL_IMX8MM_GPMI_IRQ         = 14,
    FSL_IMX8MM_CSI1_IRQ         = 16,
    FSL_IMX8MM_MIPI_CSI_IRQ     = 17,
    FSL_IMX8MM_MIPI_DSI_IRQ     = 18,
    FSL_IMX8MM_SNVS_RTC_IRQ     = 19,
    FSL_IMX8MM_SNVS_SEC_IRQ     = 20,
    FSL_IMX8MM_USDHC1_IRQ       = 22,
    FSL_IMX8MM_USDHC2_IRQ       = 23,
    FSL_IMX8MM_USDHC3_IRQ       = 24,
    FSL_IMX8MM_GPU2D_IRQ        = 25,
    FSL_IMX8MM_UART1_IRQ        = 26,
    FSL_IMX8MM_UART2_IRQ        = 27,
    FSL_IMX8MM_UART3_IRQ        = 28,
    FSL_IMX8MM_UART4_IRQ        = 29,
    FSL_IMX8MM_VPU_H1_IRQ       = 30,
    FSL_IMX8MM_ECSPI1_IRQ       = 31,
    FSL_IMX8MM_ECSPI2_IRQ       = 32,
    FSL_IMX8MM_ECSPI3_IRQ       = 33,
    FSL_IMX8MM_SDMA3_IRQ        = 34,
    FSL_IMX8MM_I2C1_IRQ         = 35,
    FSL_IMX8MM_I2C2_IRQ         = 36,
    FSL_IMX8MM_I2C3_IRQ         = 37,
    FSL_IMX8MM_I2C4_IRQ         = 38,
    FSL_IMX8MM_USB1_IRQ         = 40,
    FSL_IMX8MM_USB2_IRQ         = 41,
    FSL_IMX8MM_SYSCNT_IRQ       = 47,
    FSL_IMX8MM_TMU_IRQ          = 49,   /* TEMPMON in the NXP M4 header */
    FSL_IMX8MM_SAI3_IRQ         = 50,
    FSL_IMX8MM_GPT5_GPT6_IRQ    = 51,
    FSL_IMX8MM_GPT4_IRQ         = 52,
    FSL_IMX8MM_GPT3_IRQ         = 53,
    FSL_IMX8MM_GPT2_IRQ         = 54,
    FSL_IMX8MM_GPT1_IRQ         = 55,
    FSL_IMX8MM_GPIO1_INT7_IRQ   = 56,   /* GPIO1_INT7..INT0 are 56..63 */
    FSL_IMX8MM_GPIO1_LOW_IRQ    = 64,
    FSL_IMX8MM_GPIO1_HIGH_IRQ   = 65,
    FSL_IMX8MM_GPIO2_LOW_IRQ    = 66,
    FSL_IMX8MM_GPIO2_HIGH_IRQ   = 67,
    FSL_IMX8MM_GPIO3_LOW_IRQ    = 68,
    FSL_IMX8MM_GPIO3_HIGH_IRQ   = 69,
    FSL_IMX8MM_GPIO4_LOW_IRQ    = 70,
    FSL_IMX8MM_GPIO4_HIGH_IRQ   = 71,
    FSL_IMX8MM_GPIO5_LOW_IRQ    = 72,
    FSL_IMX8MM_GPIO5_HIGH_IRQ   = 73,
    FSL_IMX8MM_WDOG1_IRQ        = 78,
    FSL_IMX8MM_WDOG2_IRQ        = 79,
    FSL_IMX8MM_PWM1_IRQ         = 81,
    FSL_IMX8MM_PWM2_IRQ         = 82,
    FSL_IMX8MM_PWM3_IRQ         = 83,
    FSL_IMX8MM_PWM4_IRQ         = 84,
    FSL_IMX8MM_GPC_IRQ          = 87,
    FSL_IMX8MM_MU_A53_IRQ       = 88,
    FSL_IMX8MM_SRC_IRQ          = 89,
    FSL_IMX8MM_SAI5_SAI6_IRQ    = 90,
    FSL_IMX8MM_CAAM_IRQ         = 91,
    FSL_IMX8MM_SAI1_IRQ         = 95,
    FSL_IMX8MM_SAI2_IRQ         = 96,
    FSL_IMX8MM_DDR_PMU_IRQ      = 98,
    FSL_IMX8MM_SDMA2_IRQ        = 103,
    FSL_IMX8MM_QSPI_IRQ         = 107,
    FSL_IMX8MM_ENET1_IRQ        = 118,  /* MAC 0 (queue 1) */
    FSL_IMX8MM_ENET1_Q2_IRQ     = 119,  /* MAC 0 (queue 2) */
    FSL_IMX8MM_ENET1_MAC_IRQ    = 120,  /* MAC 0 (queue 0, general) */
    FSL_IMX8MM_ENET1_1588_IRQ   = 121,
    FSL_IMX8MM_PCIE1_IRQ        = 122,
};

struct FslImx8mmState {
    SysBusDevice        parent_obj;

    ARMCPU              cpu[FSL_IMX8MM_NUM_CPUS];
    GICv3State          gic;

    IMX8MMCCMState      ccm;
    IMX8MMGptClockState gpt_clk[FSL_IMX8MM_NUM_GPTS];
    IMX8MMAnatopState   anatop;
    IMX8MMSRCState      src;
    IMX8MMGPCState      gpc;
    IMX8MMIOMUXCState   iomuxc;
    IMX8MMIOMUXCGPRState iomuxc_gpr;
    IMX8MMOCOTPState    ocotp;
    IMX8MMSNVSState     snvs;
    IMX8MMTMUState      tmu;
    IMX8MMSysCtrState   sysctr;
    IMX8MMDDRCState     ddrc;
    IMX8MMApbhDmaState  apbh_dma;
    IMX8MMPWMState      pwm[4];

    IMXGPTState         gpt[FSL_IMX8MM_NUM_GPTS];
    OrIRQState          gpt5_gpt6_irq;
    IMXGPIOState        gpio[FSL_IMX8MM_NUM_GPIOS];
    IMX8MMUARTState     uart[FSL_IMX8MM_NUM_UARTS];
    IMX8MMI2CState      i2c[FSL_IMX8MM_NUM_I2CS];
    IMXSPIState         spi[FSL_IMX8MM_NUM_ECSPIS];
    IMX8MMUSDHCState    usdhc[FSL_IMX8MM_NUM_USDHCS];
    IMX8MMCQHCIState    cqhci[FSL_IMX8MM_NUM_USDHCS];
    OrIRQState          usdhc_irq[FSL_IMX8MM_NUM_USDHCS];
    IMX2WdtState        wdt[FSL_IMX8MM_NUM_WDTS];
    IMXFECState         enet;
    IMX8MMUSBState      usb[FSL_IMX8MM_NUM_USBS];

    /* Display path and GPU (docs/display-gpu.md) */
    IMX8MMDispMixState  dispmix;
    IMX8MMLCDIFState    lcdif;
    IMX8MMMipiDsimState dsim;
    VivGpuState         gpu3d;
    VivGpuState         gpu2d;

    MemoryRegion        boot_rom;
    MemoryRegion        ocram;
    MemoryRegion        ocram_s;
    MemoryRegion        tcm_itcm;
    MemoryRegion        tcm_dtcm;
    MemoryRegion        caam_mem;

    /* Properties */
    char               *cpu_type;
    uint32_t            num_cpus;
    uint64_t            cntfrq;
    bool                has_el3;
    bool                has_el2;
    bool                enet_phy_connected;
    uint32_t            sai_mclk_hz;
    char                *sdma_firmware;
    bool                sdma_infer_scripts;

    /* Optional peripherals, see fsl_imx8mm_get_optional() */
    GHashTable         *optional;
};

/*
 * Optional peripheral models are written separately. When a model with the
 * QOM type name below is compiled in, the SoC instantiates and wires it at
 * the address and interrupt from the RM; otherwise the window stays an
 * unimplemented-device. The names are the contract between the SoC and the
 * device model authors.
 */
#define TYPE_KICK_IMX8MM_SAI        "imx8mm-sai"
#define TYPE_KICK_IMX8MM_SDMA       "imx8mm-sdma"

/*
 * Look up an optional peripheral by instance name ("lcdif", "sai2", ...).
 * The objects are created when the SoC object is created, so the board can
 * set their properties before realizing the SoC. Returns NULL when the
 * model is not built in.
 */
DeviceState *fsl_imx8mm_get_optional(FslImx8mmState *s, const char *name);

#endif /* KICK_FSL_IMX8MM_H */
