/*
 * i.MX 8M Mini CCM_ANALOG (ANATOP)
 *
 * Register map from IMX8MMRM section 5.1.8 "CCM_ANALOG Memory Map". The
 * i.MX8MM has two PLL flavours:
 *
 *  - PLL1443X (audio1, audio2, video1, dram): fractional, registers
 *    GNRL_CTL, FDIV_CTL0 (MDIV/PDIV/SDIV), FDIV_CTL1 (KDIV), SSCG_CTRL,
 *    MNIT_CTRL.
 *  - PLL1416X (gpu, vpu, arm, sys1, sys2, sys3): integer, registers
 *    GNRL_CTL, DIV_CTL, LOCKD_CTRL, MNIT_CTRL.
 *
 * Fout = Fref * (MDIV + KDIV / 65536) / (PDIV * 2^SDIV).
 *
 * The lock detector is modelled as locking immediately once the PLL is
 * taken out of reset (GNRL_CTL.RST = 1), so code that polls LOCK (U-Boot
 * SPL, ATF, the Linux pll14xx driver) completes deterministically.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#include "imx8mm_anatop.h"

/* GNRL_CTL bits common to both PLL types */
#define PLL_LOCK            BIT(31)
#define PLL_RST             BIT(9)
#define PLL_BYPASS          BIT(4)
#define PLL_REF_SEL_MASK    0x3

/* 1443X: output gate and the "bypass to reference" output mux */
#define PLL1443X_CLKE       BIT(13)
#define PLL1443X_EXT_BYPASS BIT(16)
/* 1416X: output gate and the "bypass to reference" output mux */
#define PLL1416X_CLKE       BIT(11)
#define PLL1416X_EXT_BYPASS BIT(28)

#define DIV_MDIV(v)         extract32((v), 12, 10)
#define DIV_PDIV(v)         extract32((v), 4, 6)
#define DIV_SDIV(v)         extract32((v), 0, 3)

#define ANATOP_DIGPROG      0x800

typedef struct IMX8MMPllInfo {
    hwaddr gnrl;            /* offset of GNRL_CTL */
    bool frac;              /* PLL1443X (has KDIV) */
    uint32_t gnrl_reset;
    uint32_t div_reset;
    uint32_t kdiv_reset;
    const char *name;
} IMX8MMPllInfo;

/*
 * Reset state: every PLL out of reset with its output enabled, running at
 * the frequency NXP lists as the default operating point for the i.MX8MM
 * (Linux clk-imx8mm.c rate tables): audio1 393.216 MHz, audio2 361.2672 MHz,
 * video1 594 MHz, dram 750 MHz, gpu/vpu 800 MHz, arm 1.2 GHz, sys1 800 MHz,
 * sys2 1 GHz, sys3 1 GHz. SYS_PLL1/2 additionally have all fixed divider
 * outputs (DIV2..DIV20 CLKE) enabled.
 */
static const IMX8MMPllInfo imx8mm_plls[IMX8MM_PLL_COUNT] = {
    [IMX8MM_PLL_AUDIO1] = { 0x000, true,  0x00002200, 0x00106023, 0x24dd, "audio_pll1" },
    [IMX8MM_PLL_AUDIO2] = { 0x014, true,  0x00002200, 0x00169033, 0x4467, "audio_pll2" },
    [IMX8MM_PLL_VIDEO1] = { 0x028, true,  0x00002200, 0x000c6022, 0,      "video_pll1" },
    [IMX8MM_PLL_DRAM]   = { 0x050, true,  0x00002200, 0x000fa022, 0,      "dram_pll" },
    [IMX8MM_PLL_GPU]    = { 0x064, false, 0x00000a00, 0x000c8031, 0,      "gpu_pll" },
    [IMX8MM_PLL_VPU]    = { 0x074, false, 0x00000a00, 0x000c8031, 0,      "vpu_pll" },
    [IMX8MM_PLL_ARM]    = { 0x084, false, 0x00000a00, 0x0012c031, 0,      "arm_pll" },
    [IMX8MM_PLL_SYS1]   = { 0x094, false, 0x0aaaaa00, 0x000c8031, 0,      "sys_pll1" },
    [IMX8MM_PLL_SYS2]   = { 0x104, false, 0x0aaaaa00, 0x000fa031, 0,      "sys_pll2" },
    [IMX8MM_PLL_SYS3]   = { 0x114, false, 0x00000a00, 0x000fa031, 0,      "sys_pll3" },
};

#define R(off) ((off) / 4)

static bool imx8mm_anatop_is_gnrl(hwaddr offset)
{
    for (int i = 0; i < IMX8MM_PLL_COUNT; i++) {
        if (imx8mm_plls[i].gnrl == offset) {
            return true;
        }
    }
    return false;
}

/* Offsets that exist in the CCM_ANALOG register map */
static bool imx8mm_anatop_is_valid(hwaddr offset)
{
    return offset <= 0x038 ||                       /* audio1, audio2, video1 */
           (offset >= 0x050 && offset <= 0x09c) ||  /* dram .. sys1 */
           offset == 0x100 ||                       /* SYS_PLL1_MNIT_CTRL */
           (offset >= 0x104 && offset <= 0x128) ||  /* sys2, sys3, misc */
           offset == ANATOP_DIGPROG;
}

uint64_t imx8mm_anatop_pll_freq(IMX8MMAnatopState *s, IMX8MMPll pll)
{
    const IMX8MMPllInfo *info = &imx8mm_plls[pll];
    uint32_t gnrl = s->regs[R(info->gnrl)];
    uint32_t div = s->regs[R(info->gnrl + 4)];
    uint32_t clke = info->frac ? PLL1443X_CLKE : PLL1416X_CLKE;
    uint32_t ext_bypass = info->frac ? PLL1443X_EXT_BYPASS :
                                       PLL1416X_EXT_BYPASS;
    uint64_t fref, fvco;
    uint32_t mdiv, pdiv, sdiv;
    int16_t kdiv = 0;

    /* Only the 24 MHz oscillator is a valid reference (others are "dummy") */
    fref = (gnrl & PLL_REF_SEL_MASK) == 0 ? s->osc24m_freq : 0;

    if (!(gnrl & clke)) {
        return 0;
    }
    if ((gnrl & ext_bypass) || (gnrl & PLL_BYPASS)) {
        return fref;
    }
    if (!(gnrl & PLL_RST)) {
        /* RST = 0 holds the PLL in reset: no output */
        return 0;
    }

    mdiv = DIV_MDIV(div);
    pdiv = DIV_PDIV(div);
    sdiv = DIV_SDIV(div);
    if (info->frac) {
        kdiv = (int16_t)extract32(s->regs[R(info->gnrl + 8)], 0, 16);
    }
    if (pdiv == 0) {
        return 0;
    }

    fvco = fref * ((uint64_t)mdiv * 65536 + kdiv) / ((uint64_t)pdiv * 65536);
    return fvco >> sdiv;
}

static uint64_t imx8mm_anatop_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMAnatopState *s = IMX8MM_ANATOP(opaque);
    uint32_t value;

    if (!imx8mm_anatop_is_valid(offset)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_ANATOP, offset);
        return 0;
    }

    value = s->regs[R(offset)];
    if (imx8mm_anatop_is_gnrl(offset)) {
        /* LOCK follows RST: the PLL locks as soon as it leaves reset */
        value &= ~PLL_LOCK;
        if (value & PLL_RST) {
            value |= PLL_LOCK;
        }
    }
    return value;
}

static void imx8mm_anatop_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    IMX8MMAnatopState *s = IMX8MM_ANATOP(opaque);

    if (!imx8mm_anatop_is_valid(offset)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_ANATOP, offset);
        return;
    }
    if (offset == ANATOP_DIGPROG) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DIGPROG is read-only\n",
                      TYPE_IMX8MM_ANATOP);
        return;
    }
    if (imx8mm_anatop_is_gnrl(offset)) {
        value &= ~PLL_LOCK;     /* status bit, read-only */
    }
    s->regs[R(offset)] = value;
}

static const MemoryRegionOps imx8mm_anatop_ops = {
    .read = imx8mm_anatop_read,
    .write = imx8mm_anatop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_anatop_reset(DeviceState *dev)
{
    IMX8MMAnatopState *s = IMX8MM_ANATOP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (int i = 0; i < IMX8MM_PLL_COUNT; i++) {
        const IMX8MMPllInfo *info = &imx8mm_plls[i];

        s->regs[R(info->gnrl)] = info->gnrl_reset;
        s->regs[R(info->gnrl + 4)] = info->div_reset;
        if (info->frac) {
            s->regs[R(info->gnrl + 8)] = info->kdiv_reset;
        }
    }
    s->regs[R(ANATOP_DIGPROG)] = s->digprog;
}

static void imx8mm_anatop_init(Object *obj)
{
    IMX8MMAnatopState *s = IMX8MM_ANATOP(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_anatop_ops, s,
                          TYPE_IMX8MM_ANATOP, IMX8MM_ANATOP_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription imx8mm_anatop_vmstate = {
    .name = TYPE_IMX8MM_ANATOP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMAnatopState, IMX8MM_ANATOP_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx8mm_anatop_properties[] = {
    /*
     * DIGPROG: [23:16] 0x82 = i.MX8M family, [15:8] 0x41 = Mini,
     * [7:0] 0x10 = silicon revision 1.0 (Linux soc-imx8m, U-Boot get_cpu_rev).
     */
    DEFINE_PROP_UINT32("digprog", IMX8MMAnatopState, digprog, 0x00824110),
    DEFINE_PROP_UINT32("osc24m-freq", IMX8MMAnatopState, osc24m_freq,
                       24000000),
};

static void imx8mm_anatop_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_anatop_reset);
    device_class_set_props(dc, imx8mm_anatop_properties);
    dc->vmsd = &imx8mm_anatop_vmstate;
    dc->desc = "i.MX8MM CCM_ANALOG (PLLs)";
}

static const TypeInfo imx8mm_anatop_types[] = {
    {
        .name          = TYPE_IMX8MM_ANATOP,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMAnatopState),
        .instance_init = imx8mm_anatop_init,
        .class_init    = imx8mm_anatop_class_init,
    },
};

DEFINE_TYPES(imx8mm_anatop_types)
