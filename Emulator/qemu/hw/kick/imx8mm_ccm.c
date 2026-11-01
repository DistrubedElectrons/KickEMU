/*
 * i.MX 8M Mini Clock Control Module (CCM)
 *
 * Register layout (IMX8MMRM section 5.1.7 "CCM Memory Map"):
 *
 *   0x0000          CCM_GPR0
 *   0x0800 + n*0x10 CCM_PLL_CTRLn   (n = 0..38), PLL output gating
 *   0x4000 + n*0x10 CCM_CCGRn       clock gates, 2-bit per domain fields
 *   0x8000 + n*0x80 CCM_TARGET_ROOTn  clock root slices, followed by
 *                   +0x10 POST_ROOT, +0x20 PRE_ROOT, +0x30 ACCESS_CTRL
 *
 * Every register has SET (+0x4), CLR (+0x8) and TOG (+0xC) aliases.
 *
 * TARGET_ROOT fields: ENABLE [28], MUX [26:24], PRE_PODF [18:16],
 * POST_PODF [5:0] (bus/IP slices) or [2:0] (core slices).
 *
 * The model keeps the register file and computes clock root frequencies
 * from it and from the ANATOP PLLs, which the GPT model uses for its
 * ipg_clk inputs. POST_ROOT/PRE_ROOT are kept as plain storage; neither
 * U-Boot nor Linux use them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

#include "imx8mm_ccm.h"

#define CCM_GPR0            0x0000
#define CCM_PLL_CTRL_BASE   0x0800
#define CCM_PLL_CTRL_END    0x0a70
#define CCM_CCGR_BASE       0x4000
#define CCM_CCGR_END        0x5ff0
#define CCM_ROOT_BASE       0x8000
#define CCM_ROOT_END        0xc2ff

#define ROOT_ENABLE         BIT(28)
#define ROOT_MUX(v)         extract32((v), 24, 3)
#define ROOT_PRE_PODF(v)    extract32((v), 16, 3)
#define ROOT_POST_PODF(v)   extract32((v), 0, 6)
#define ROOT_CORE_PODF(v)   extract32((v), 0, 3)

/* Reset values (see the comment in imx8mm_ccm_reset) */
#define CCGR_RESET          0x00000003
#define PLL_CTRL_RESET      0x00000003
#define ROOT_RESET          ROOT_ENABLE

typedef enum IMX8MMClockSource {
    IMX8MM_SRC_DUMMY,
    IMX8MM_SRC_OSC_24M,
    IMX8MM_SRC_OSC_32K,
    IMX8MM_SRC_OSC_27M,
    IMX8MM_SRC_OSC_HDMI,
    IMX8MM_SRC_CLK_EXT1,
    IMX8MM_SRC_CLK_EXT2,
    IMX8MM_SRC_CLK_EXT3,
    IMX8MM_SRC_CLK_EXT4,
    IMX8MM_SRC_ARM_PLL_OUT,
    IMX8MM_SRC_GPU_PLL_OUT,
    IMX8MM_SRC_VPU_PLL_OUT,
    IMX8MM_SRC_DRAM_PLL_OUT,
    IMX8MM_SRC_AUDIO_PLL1_OUT,
    IMX8MM_SRC_AUDIO_PLL2_OUT,
    IMX8MM_SRC_VIDEO_PLL1_OUT,
    IMX8MM_SRC_SYS_PLL3_OUT,
    IMX8MM_SRC_SYS_PLL1_40M,
    IMX8MM_SRC_SYS_PLL1_80M,
    IMX8MM_SRC_SYS_PLL1_100M,
    IMX8MM_SRC_SYS_PLL1_133M,
    IMX8MM_SRC_SYS_PLL1_160M,
    IMX8MM_SRC_SYS_PLL1_200M,
    IMX8MM_SRC_SYS_PLL1_266M,
    IMX8MM_SRC_SYS_PLL1_400M,
    IMX8MM_SRC_SYS_PLL1_800M,
    IMX8MM_SRC_SYS_PLL2_50M,
    IMX8MM_SRC_SYS_PLL2_100M,
    IMX8MM_SRC_SYS_PLL2_125M,
    IMX8MM_SRC_SYS_PLL2_166M,
    IMX8MM_SRC_SYS_PLL2_200M,
    IMX8MM_SRC_SYS_PLL2_250M,
    IMX8MM_SRC_SYS_PLL2_333M,
    IMX8MM_SRC_SYS_PLL2_500M,
    IMX8MM_SRC_SYS_PLL2_1000M,
    IMX8MM_SRC_COUNT,
} IMX8MMClockSource;

/*
 * A mux input is either a fixed source (< 0x100) or another clock root,
 * encoded as 0x100 + index into imx8mm_clock_roots[] (0x200 + index for the
 * fixed /4 "dram_alt_root").
 */
#define IMX8MM_CLK_ROOT(i)      (0x100 + (i))
#define IMX8MM_CLK_ROOT_DIV4(i) (0x200 + (i))

typedef enum IMX8MMRootKind {
    IMX8MM_ROOT_CORE,       /* core slice: mux + 3-bit post divider */
    IMX8MM_ROOT_COMPOSITE,  /* bus/IP slice: mux + pre + post divider */
    IMX8MM_ROOT_IPG,        /* IPG_ROOT style: 1-bit divider of its parent */
    IMX8MM_ROOT_MUX1,       /* 1-bit glitchless mux (ARM_A53_CORE, DRAM) */
} IMX8MMRootKind;

typedef struct IMX8MMClockRoot {
    uint16_t offset;
    const char *name;
    IMX8MMRootKind kind;
    uint16_t parents[8];
} IMX8MMClockRoot;

#include "imx8mm_ccm_roots.c.inc"

#define REG(off)            ((off) / 16)
#define BITOP(off)          ((off) & 0xc)

static int imx8mm_ccm_root_index(hwaddr offset)
{
    for (int i = 0; i < ARRAY_SIZE(imx8mm_clock_roots); i++) {
        if (imx8mm_clock_roots[i].offset == offset) {
            return i;
        }
    }
    return -1;
}

static uint64_t imx8mm_ccm_index_freq(IMX8MMCCMState *s, int idx, int depth);

static uint64_t imx8mm_ccm_source_freq(IMX8MMCCMState *s, uint16_t src,
                                       int depth)
{
    IMX8MMAnatopState *a = s->anatop;

    if (src >= 0x200) {
        return imx8mm_ccm_index_freq(s, src - 0x200, depth + 1) / 4;
    }
    if (src >= 0x100) {
        return imx8mm_ccm_index_freq(s, src - 0x100, depth + 1);
    }

    switch ((IMX8MMClockSource)src) {
    case IMX8MM_SRC_OSC_24M:
        return a->osc24m_freq;
    case IMX8MM_SRC_OSC_32K:
        return s->osc32k_freq;
    case IMX8MM_SRC_OSC_27M:
        return s->osc27m_freq;
    case IMX8MM_SRC_CLK_EXT1 ... IMX8MM_SRC_CLK_EXT4:
        return s->ext_freq[src - IMX8MM_SRC_CLK_EXT1];
    case IMX8MM_SRC_ARM_PLL_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_ARM);
    case IMX8MM_SRC_GPU_PLL_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_GPU);
    case IMX8MM_SRC_VPU_PLL_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_VPU);
    case IMX8MM_SRC_DRAM_PLL_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_DRAM);
    case IMX8MM_SRC_AUDIO_PLL1_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_AUDIO1);
    case IMX8MM_SRC_AUDIO_PLL2_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_AUDIO2);
    case IMX8MM_SRC_VIDEO_PLL1_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_VIDEO1);
    case IMX8MM_SRC_SYS_PLL3_OUT:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS3);
    /* SYS_PLL1/2 fixed post dividers (RM figure 5-1) */
    case IMX8MM_SRC_SYS_PLL1_40M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 20;
    case IMX8MM_SRC_SYS_PLL1_80M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 10;
    case IMX8MM_SRC_SYS_PLL1_100M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 8;
    case IMX8MM_SRC_SYS_PLL1_133M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 6;
    case IMX8MM_SRC_SYS_PLL1_160M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 5;
    case IMX8MM_SRC_SYS_PLL1_200M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 4;
    case IMX8MM_SRC_SYS_PLL1_266M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 3;
    case IMX8MM_SRC_SYS_PLL1_400M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1) / 2;
    case IMX8MM_SRC_SYS_PLL1_800M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS1);
    case IMX8MM_SRC_SYS_PLL2_50M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 20;
    case IMX8MM_SRC_SYS_PLL2_100M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 10;
    case IMX8MM_SRC_SYS_PLL2_125M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 8;
    case IMX8MM_SRC_SYS_PLL2_166M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 6;
    case IMX8MM_SRC_SYS_PLL2_200M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 5;
    case IMX8MM_SRC_SYS_PLL2_250M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 4;
    case IMX8MM_SRC_SYS_PLL2_333M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 3;
    case IMX8MM_SRC_SYS_PLL2_500M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2) / 2;
    case IMX8MM_SRC_SYS_PLL2_1000M:
        return imx8mm_anatop_pll_freq(a, IMX8MM_PLL_SYS2);
    case IMX8MM_SRC_DUMMY:
    case IMX8MM_SRC_OSC_HDMI:   /* no HDMI PHY on the 8MM */
    default:
        return 0;
    }
}

static uint64_t imx8mm_ccm_index_freq(IMX8MMCCMState *s, int idx, int depth)
{
    const IMX8MMClockRoot *root = &imx8mm_clock_roots[idx];
    uint32_t v = s->regs[REG(root->offset)];
    uint64_t parent;

    if (depth > 4) {
        return 0;   /* roots chain at most twice, guard anyway */
    }

    switch (root->kind) {
    case IMX8MM_ROOT_CORE:
        if (!(v & ROOT_ENABLE)) {
            return 0;
        }
        parent = imx8mm_ccm_source_freq(s, root->parents[ROOT_MUX(v)], depth);
        return parent / (ROOT_CORE_PODF(v) + 1);
    case IMX8MM_ROOT_COMPOSITE:
        if (!(v & ROOT_ENABLE)) {
            return 0;
        }
        parent = imx8mm_ccm_source_freq(s, root->parents[ROOT_MUX(v)], depth);
        return parent / (ROOT_PRE_PODF(v) + 1) / (ROOT_POST_PODF(v) + 1);
    case IMX8MM_ROOT_IPG:
        parent = imx8mm_ccm_source_freq(s, root->parents[0], depth);
        return parent / (extract32(v, 0, 1) + 1);
    case IMX8MM_ROOT_MUX1:
        return imx8mm_ccm_source_freq(s, root->parents[extract32(v, 24, 1)],
                                      depth);
    }
    return 0;
}

uint64_t imx8mm_ccm_root_freq(IMX8MMCCMState *s, hwaddr offset)
{
    int idx = imx8mm_ccm_root_index(offset);

    return idx < 0 ? 0 : imx8mm_ccm_index_freq(s, idx, 0);
}

static bool imx8mm_ccm_is_valid(hwaddr offset)
{
    hwaddr reg = offset & ~0xfULL;

    if (reg == CCM_GPR0) {
        return true;
    }
    if (reg >= CCM_PLL_CTRL_BASE && reg <= CCM_PLL_CTRL_END) {
        return true;
    }
    if (reg >= CCM_CCGR_BASE && reg <= CCM_CCGR_END) {
        return true;
    }
    if (reg >= CCM_ROOT_BASE && reg <= CCM_ROOT_END) {
        /* TARGET, POST, PRE and ACCESS_CTRL; +0x40..+0x7f is reserved */
        return (reg & 0x7f) < 0x40;
    }
    return false;
}

static uint64_t imx8mm_ccm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMCCMState *s = IMX8MM_CCM(opaque);

    if (!imx8mm_ccm_is_valid(offset)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_CCM, offset);
        return 0;
    }
    /* The SET/CLR/TOG aliases read back the register value */
    return s->regs[REG(offset)];
}

static void imx8mm_ccm_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX8MMCCMState *s = IMX8MM_CCM(opaque);
    uint32_t *reg = &s->regs[REG(offset)];

    if (!imx8mm_ccm_is_valid(offset)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_CCM, offset);
        return;
    }

    switch (BITOP(offset)) {
    case 0x0:
        *reg = value;
        break;
    case 0x4:
        *reg |= value;
        break;
    case 0x8:
        *reg &= ~value;
        break;
    case 0xc:
        *reg ^= value;
        break;
    }
}

static const MemoryRegionOps imx8mm_ccm_ops = {
    .read = imx8mm_ccm_read,
    .write = imx8mm_ccm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_ccm_reset(DeviceState *dev)
{
    IMX8MMCCMState *s = IMX8MM_CCM(dev);

    /*
     * Reset state: all CCGR gates and PLL_CTRL gates set to "needed in
     * domain 0" (0x3), every clock root enabled on mux input 0 (24 MHz
     * oscillator for nearly all slices) with dividers at /1. This matches
     * what the BootROM leaves for the blocks SPL touches first; SPL and
     * Linux reprogram every root they use.
     */
    memset(s->regs, 0, sizeof(s->regs));
    for (hwaddr off = CCM_PLL_CTRL_BASE; off <= CCM_PLL_CTRL_END; off += 16) {
        s->regs[REG(off)] = PLL_CTRL_RESET;
    }
    for (hwaddr off = CCM_CCGR_BASE; off <= CCM_CCGR_END; off += 16) {
        s->regs[REG(off)] = CCGR_RESET;
    }
    for (int i = 0; i < ARRAY_SIZE(imx8mm_clock_roots); i++) {
        const IMX8MMClockRoot *root = &imx8mm_clock_roots[i];

        if (root->kind == IMX8MM_ROOT_CORE ||
            root->kind == IMX8MM_ROOT_COMPOSITE) {
            s->regs[REG(root->offset)] = ROOT_RESET;
        }
    }
}

static uint32_t imx8mm_ccm_get_clock_frequency(IMXCCMState *dev, IMXClk clock)
{
    IMX8MMCCMState *s = IMX8MM_CCM(dev);

    switch (clock) {
    case CLK_32k:
        return s->osc32k_freq;
    case CLK_HIGH:
        return s->anatop->osc24m_freq;
    case CLK_IPG:
    case CLK_IPG_HIGH:
        return imx8mm_ccm_root_freq(s, 0x9080);     /* IPG_ROOT */
    case CLK_EXT:
        return s->ext_freq[0];
    default:
        return 0;
    }
}

static void imx8mm_ccm_init(Object *obj)
{
    IMX8MMCCMState *s = IMX8MM_CCM(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_ccm_ops, s,
                          TYPE_IMX8MM_CCM, IMX8MM_CCM_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void imx8mm_ccm_realize(DeviceState *dev, Error **errp)
{
    IMX8MMCCMState *s = IMX8MM_CCM(dev);

    if (!s->anatop) {
        error_setg(errp, "%s: 'anatop' link not set", TYPE_IMX8MM_CCM);
    }
}

static const VMStateDescription imx8mm_ccm_vmstate = {
    .name = TYPE_IMX8MM_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMCCMState, IMX8MM_CCM_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx8mm_ccm_properties[] = {
    DEFINE_PROP_LINK("anatop", IMX8MMCCMState, anatop, TYPE_IMX8MM_ANATOP,
                     IMX8MMAnatopState *),
    DEFINE_PROP_UINT32("osc32k-freq", IMX8MMCCMState, osc32k_freq, 32768),
    DEFINE_PROP_UINT32("osc27m-freq", IMX8MMCCMState, osc27m_freq, 27000000),
    /* CLK_EXT1..4 pads: not driven on the VAR-SOM-MX8M-MINI */
    DEFINE_PROP_UINT32("clk-ext1-freq", IMX8MMCCMState, ext_freq[0], 0),
    DEFINE_PROP_UINT32("clk-ext2-freq", IMX8MMCCMState, ext_freq[1], 0),
    DEFINE_PROP_UINT32("clk-ext3-freq", IMX8MMCCMState, ext_freq[2], 0),
    DEFINE_PROP_UINT32("clk-ext4-freq", IMX8MMCCMState, ext_freq[3], 0),
};

static void imx8mm_ccm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IMXCCMClass *ccm = IMX_CCM_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_ccm_reset);
    device_class_set_props(dc, imx8mm_ccm_properties);
    dc->realize = imx8mm_ccm_realize;
    dc->vmsd = &imx8mm_ccm_vmstate;
    dc->desc = "i.MX8MM Clock Control Module";
    ccm->get_clock_frequency = imx8mm_ccm_get_clock_frequency;
}

/* GPT clock view, see imx8mm_ccm.h */

static uint32_t imx8mm_gpt_clock_get_frequency(IMXCCMState *dev, IMXClk clock)
{
    IMX8MMGptClockState *g = IMX8MM_GPT_CLOCK(dev);

    switch (clock) {
    case CLK_IPG:
    case CLK_IPG_HIGH:
        /* Both GPT functional clock inputs come from GPTn_CLK_ROOT */
        return imx8mm_ccm_root_freq(g->ccm, g->root_offset);
    default:
        return imx_ccm_get_clock_frequency(IMX_CCM(g->ccm), clock);
    }
}

static void imx8mm_gpt_clock_realize(DeviceState *dev, Error **errp)
{
    IMX8MMGptClockState *g = IMX8MM_GPT_CLOCK(dev);

    if (!g->ccm) {
        error_setg(errp, "%s: 'ccm' link not set", TYPE_IMX8MM_GPT_CLOCK);
        return;
    }
    if (imx8mm_ccm_root_index(g->root_offset) < 0) {
        error_setg(errp, "%s: 0x%x is not a clock root",
                   TYPE_IMX8MM_GPT_CLOCK, g->root_offset);
    }
}

static const Property imx8mm_gpt_clock_properties[] = {
    DEFINE_PROP_LINK("ccm", IMX8MMGptClockState, ccm, TYPE_IMX8MM_CCM,
                     IMX8MMCCMState *),
    DEFINE_PROP_UINT32("root-offset", IMX8MMGptClockState, root_offset,
                       IMX8MM_CCM_ROOT_GPT1),
};

static void imx8mm_gpt_clock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IMXCCMClass *ccm = IMX_CCM_CLASS(klass);

    device_class_set_props(dc, imx8mm_gpt_clock_properties);
    dc->realize = imx8mm_gpt_clock_realize;
    dc->desc = "i.MX8MM GPT clock root view";
    ccm->get_clock_frequency = imx8mm_gpt_clock_get_frequency;
}

static const TypeInfo imx8mm_ccm_types[] = {
    {
        .name          = TYPE_IMX8MM_CCM,
        .parent        = TYPE_IMX_CCM,
        .instance_size = sizeof(IMX8MMCCMState),
        .instance_init = imx8mm_ccm_init,
        .class_init    = imx8mm_ccm_class_init,
    },
    {
        .name          = TYPE_IMX8MM_GPT_CLOCK,
        .parent        = TYPE_IMX_CCM,
        .instance_size = sizeof(IMX8MMGptClockState),
        .class_init    = imx8mm_gpt_clock_class_init,
    },
};

DEFINE_TYPES(imx8mm_ccm_types)
