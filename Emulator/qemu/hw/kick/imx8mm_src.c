/*
 * i.MX 8M Mini System Reset Controller (SRC)
 *
 * IMX8MMRM section 6.2 "System Reset Controller (SRC)". Besides plain
 * storage for the module reset registers, the model implements:
 *
 *  - SRSR (reset status) as write-1-to-clear, POR after reset.
 *  - SBMR1/SBMR2 (boot mode and boot configuration fuses) read-only from
 *    properties, so SPL sees the boot device the board was wired for.
 *  - A53 secondary core release, the sequence ATF uses for PSCI CPU_ON
 *    (plat/imx/imx8m/gpc_common.c): the entry point is written to
 *    SRC_GPR1 + core * 8 (two words, see imx8mm_src_cpu_entry), and the
 *    core starts when its A53RCR1.nCPUPORESET bit goes from 0 to 1.
 *    Clearing the bit again powers the core off.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "target/arm/arm-powerctl.h"
#include "target/arm/cpu.h"

#include "imx8mm_src.h"

#define SRC_SCR         0x00
#define SRC_A53RCR0     0x04
#define SRC_A53RCR1     0x08
#define SRC_M4RCR       0x0c
#define SRC_SBMR1       0x58
#define SRC_SRSR        0x5c
#define SRC_SISR        0x68
#define SRC_SIMR        0x6c
#define SRC_SBMR2       0x70
#define SRC_GPR1        0x74
#define SRC_GPR10       0x98
#define SRC_DDRC_RCR    0x1000
#define SRC_DDRC2_RCR   0x1004

/* SRSR: ipp_reset_b (power-on reset) after a cold boot */
#define SRSR_POR        BIT(0)
#define SRSR_W1C_MASK   0x000003ff

#define R(off)          ((off) / 4)

static void imx8mm_src_update_irq(IMX8MMSRCState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[R(SRC_SISR)] & ~s->regs[R(SRC_SIMR)]));
}

/*
 * Entry address of core @n as programmed by imx_set_cpu_entry(): the
 * address shifted right by 2 is split into a 16-bit high word (bits [37:22]
 * of the shifted value) at GPR1 + 8n and a 22-bit low word at GPR1 + 8n + 4.
 */
static uint64_t imx8mm_src_cpu_entry(IMX8MMSRCState *s, int n)
{
    uint64_t hi = s->regs[R(SRC_GPR1 + n * 8)] & 0xffff;
    uint64_t lo = s->regs[R(SRC_GPR1 + n * 8 + 4)] & 0x3fffff;

    return ((hi << 22) | lo) << 2;
}

static void imx8mm_src_a53rcr1_write(IMX8MMSRCState *s, uint32_t value)
{
    uint32_t old = s->regs[R(SRC_A53RCR1)];
    uint32_t changed = old ^ value;

    s->regs[R(SRC_A53RCR1)] = value;

    /* Core 0 is the boot core and is never released or held through here */
    for (int n = 1; n < s->num_cpus; n++) {
        CPUState *cs;
        ARMCPU *cpu;
        int el;

        if (!(changed & BIT(n))) {
            continue;
        }
        cs = arm_get_cpu_by_id(n);
        if (!cs) {
            continue;
        }
        cpu = ARM_CPU(cs);

        if (value & BIT(n)) {
            uint64_t entry = imx8mm_src_cpu_entry(s, n);

            el = arm_feature(&cpu->env, ARM_FEATURE_EL3) ? 3 :
                 arm_feature(&cpu->env, ARM_FEATURE_EL2) ? 2 : 1;
            if (arm_set_cpu_on(n, entry, 0, el, true) !=
                QEMU_ARM_POWERCTL_RET_SUCCESS) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to release core %d"
                              " at 0x%" PRIx64 "\n", TYPE_IMX8MM_SRC, n,
                              entry);
            }
        } else {
            arm_set_cpu_off(n);
        }
    }
}

static uint64_t imx8mm_src_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMSRCState *s = IMX8MM_SRC(opaque);

    switch (offset) {
    case SRC_SBMR1:
        return s->sbmr1;
    case SRC_SBMR2:
        return s->sbmr2;
    case SRC_DDRC_RCR:
    case SRC_DDRC2_RCR:
        return s->ddr_regs[(offset - SRC_DDRC_RCR) / 4];
    default:
        if (offset < SRC_GPR10 + 4) {
            return s->regs[R(offset)];
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_SRC, offset);
        return 0;
    }
}

static void imx8mm_src_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX8MMSRCState *s = IMX8MM_SRC(opaque);

    switch (offset) {
    case SRC_SBMR1:
    case SRC_SBMR2:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: SBMR is read-only\n",
                      TYPE_IMX8MM_SRC);
        break;
    case SRC_SRSR:
        s->regs[R(offset)] &= ~(value & SRSR_W1C_MASK);
        break;
    case SRC_SISR:
        s->regs[R(offset)] &= ~value;   /* W1C */
        imx8mm_src_update_irq(s);
        break;
    case SRC_SIMR:
        s->regs[R(offset)] = value;
        imx8mm_src_update_irq(s);
        break;
    case SRC_A53RCR1:
        imx8mm_src_a53rcr1_write(s, value);
        break;
    case SRC_DDRC_RCR:
    case SRC_DDRC2_RCR:
        s->ddr_regs[(offset - SRC_DDRC_RCR) / 4] = value;
        break;
    default:
        if (offset < SRC_GPR10 + 4) {
            s->regs[R(offset)] = value;
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_SRC, offset);
        break;
    }
}

static const MemoryRegionOps imx8mm_src_ops = {
    .read = imx8mm_src_read,
    .write = imx8mm_src_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_src_reset(DeviceState *dev)
{
    IMX8MMSRCState *s = IMX8MM_SRC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R(SRC_SCR)] = 0x000000a0;
    s->regs[R(SRC_A53RCR1)] = 0x00000001;   /* only core 0 out of reset */
    s->regs[R(SRC_SRSR)] = SRSR_POR;
    s->regs[R(SRC_SIMR)] = 0x0000001f;      /* all reset interrupts masked */
    /* DDR controller and PHY held in reset until SPL releases them */
    s->ddr_regs[0] = 0x8f00000f;
    s->ddr_regs[1] = 0x8f00000f;
}

static void imx8mm_src_init(Object *obj)
{
    IMX8MMSRCState *s = IMX8MM_SRC(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_src_ops, s,
                          TYPE_IMX8MM_SRC, IMX8MM_SRC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription imx8mm_src_vmstate = {
    .name = TYPE_IMX8MM_SRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMSRCState, IMX8MM_SRC_NUM_REGS),
        VMSTATE_UINT32_ARRAY(ddr_regs, IMX8MMSRCState,
                             IMX8MM_SRC_NUM_DDR_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx8mm_src_properties[] = {
    /* Boot configuration fuses as sampled at POR (BOOT_CFG[15:0] etc.) */
    DEFINE_PROP_UINT32("sbmr1", IMX8MMSRCState, sbmr1, 0),
    /* SBMR2: [25:24] BMOD (0b10 = internal boot), [4] BT_FUSE_SEL */
    DEFINE_PROP_UINT32("sbmr2", IMX8MMSRCState, sbmr2, 0x02000000),
    DEFINE_PROP_UINT32("num-cpus", IMX8MMSRCState, num_cpus, 4),
};

static void imx8mm_src_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_src_reset);
    device_class_set_props(dc, imx8mm_src_properties);
    dc->vmsd = &imx8mm_src_vmstate;
    dc->desc = "i.MX8MM System Reset Controller";
}

static const TypeInfo imx8mm_src_types[] = {
    {
        .name          = TYPE_IMX8MM_SRC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMSRCState),
        .instance_init = imx8mm_src_init,
        .class_init    = imx8mm_src_class_init,
    },
};

DEFINE_TYPES(imx8mm_src_types)
