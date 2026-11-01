/*
 * i.MX 8M Mini CCM_ANALOG (ANATOP): PLLs, oscillator control, DIGPROG
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_ANATOP_H
#define KICK_IMX8MM_ANATOP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX8MM_ANATOP "imx8mm.anatop"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMAnatopState, IMX8MM_ANATOP)

/* Register file covers 0x000..0x800 (DIGPROG is the last register) */
#define IMX8MM_ANATOP_MMIO_SIZE     0x10000
#define IMX8MM_ANATOP_NUM_REGS      ((0x800 / 4) + 1)

/* PLLs of the i.MX8MM (RM 5.1.5 "PLLs") */
typedef enum IMX8MMPll {
    IMX8MM_PLL_AUDIO1,
    IMX8MM_PLL_AUDIO2,
    IMX8MM_PLL_VIDEO1,
    IMX8MM_PLL_DRAM,
    IMX8MM_PLL_GPU,
    IMX8MM_PLL_VPU,
    IMX8MM_PLL_ARM,
    IMX8MM_PLL_SYS1,
    IMX8MM_PLL_SYS2,
    IMX8MM_PLL_SYS3,
    IMX8MM_PLL_COUNT
} IMX8MMPll;

struct IMX8MMAnatopState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX8MM_ANATOP_NUM_REGS];

    /* Properties */
    uint32_t digprog;
    uint32_t osc24m_freq;
};

/*
 * Output frequency of a PLL in Hz after its bypass mux and output gate,
 * the value a clock root sees when it selects "<pll>_out".
 */
uint64_t imx8mm_anatop_pll_freq(IMX8MMAnatopState *s, IMX8MMPll pll);

#endif /* KICK_IMX8MM_ANATOP_H */
