/*
 * i.MX 8M Mini Clock Control Module (CCM)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_CCM_H
#define KICK_IMX8MM_CCM_H

#include "hw/misc/imx_ccm.h"
#include "qom/object.h"

#include "imx8mm_anatop.h"

#define TYPE_IMX8MM_CCM "imx8mm.ccm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMCCMState, IMX8MM_CCM)

/*
 * The CCM decodes 64 KiB. Every register is a 16-byte group:
 * +0x0 value, +0x4 SET, +0x8 CLR, +0xC TOG.
 */
#define IMX8MM_CCM_MMIO_SIZE    0x10000
#define IMX8MM_CCM_NUM_REGS     (IMX8MM_CCM_MMIO_SIZE / 16)

struct IMX8MMCCMState {
    IMXCCMState parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX8MM_CCM_NUM_REGS];

    IMX8MMAnatopState *anatop;
    uint32_t osc32k_freq;
    uint32_t osc27m_freq;
    uint32_t ext_freq[4];
};

/*
 * Frequency in Hz of the clock root (slice) whose TARGET_ROOT register is at
 * @offset (0x8000..0xc280), or 0 if the offset is not a clock root.
 */
uint64_t imx8mm_ccm_root_freq(IMX8MMCCMState *s, hwaddr offset);

/* CCM_TARGET_ROOT offsets used by the SoC */
#define IMX8MM_CCM_ROOT_GPT1    0xb580  /* GPT2..GPT6 follow at +0x80 */

/*
 * Per-GPT clock view. The upstream GPT model asks its IMXCCMState for
 * "ipg_clk"/"ipg_clk_highfreq" without saying which GPT it is; on the 8MM
 * those are the per-instance GPTn_CLK_ROOT slices. Each GPT gets one of these
 * small objects so CLKSRC=1/2 resolves to its own clock root.
 */
#define TYPE_IMX8MM_GPT_CLOCK "imx8mm.gpt-clock"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMGptClockState, IMX8MM_GPT_CLOCK)

struct IMX8MMGptClockState {
    IMXCCMState parent_obj;

    IMX8MMCCMState *ccm;
    uint32_t root_offset;
};

#endif /* KICK_IMX8MM_CCM_H */
