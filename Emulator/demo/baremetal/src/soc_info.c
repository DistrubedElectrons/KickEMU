/*
 * SoC identification the way U-Boot (arch/arm/mach-imx/imx8m/soc.c) and
 * Linux (drivers/soc/imx/soc-imx8m.c) do it:
 *
 *   CCM_ANALOG DIGPROG (0x30360800): [23:16] 0x82 = i.MX8M family,
 *     [15:8] 0x41 = Mini (0x42 Nano, 0x43 Plus), [7:0] silicon revision
 *   OCOTP TESTER0/1 (0x410/0x420): 64-bit unique ID
 *   OCOTP TESTER4 (0x450): fused-off cores / VPU (variant)
 *   OCOTP MAC0 (0x640/0x650): Ethernet MAC address
 *
 * The TMU gives the die temperature after TER.EN is set.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "imx8mm.h"
#include "soc_info.h"

#define ANA_DIGPROG         0x800
#define OCOTP_SHADOW(n)     (0x400 + (n) * 0x10)

#define TMU_TER             0x00
#define TMU_TRITSR          0x20
#define TMU_TER_EN          (1u << 31)

void soc_info_read(soc_info *info)
{
    uint32_t tester4, mac0, mac1;

    info->digprog = rd32(ANATOP_BASE + ANA_DIGPROG);
    switch ((info->digprog >> 8) & 0xff) {
    case 0x41:
        info->name = "i.MX8MM";
        break;
    case 0x42:
        info->name = "i.MX8MN";
        break;
    case 0x43:
        info->name = "i.MX8MP";
        break;
    default:
        info->name = "i.MX8MQ";
        break;
    }
    info->rev_major = (info->digprog >> 4) & 0xf;
    info->rev_minor = info->digprog & 0xf;

    info->uid = (uint64_t)rd32(OCOTP_BASE + OCOTP_SHADOW(2)) << 32 |
                rd32(OCOTP_BASE + OCOTP_SHADOW(1));
    info->speed_grade = rd32(OCOTP_BASE + OCOTP_SHADOW(4));

    /* Same decoding as U-Boot get_cpu_variant_type() for the 8MM */
    tester4 = rd32(OCOTP_BASE + OCOTP_SHADOW(5));
    switch (tester4 & 0x3) {
    case 2:
        info->variant = (tester4 & 0x1c0000) ? "DualLite" : "Dual";
        break;
    case 3:
        info->variant = (tester4 & 0x1c0000) ? "SoloLite" : "Solo";
        break;
    default:
        info->variant = (tester4 & 0x1c0000) ? "QuadLite" : "Quad";
        break;
    }

    mac0 = rd32(OCOTP_BASE + OCOTP_SHADOW(36));
    mac1 = rd32(OCOTP_BASE + OCOTP_SHADOW(37));
    info->mac[0] = mac1 >> 8;
    info->mac[1] = mac1;
    info->mac[2] = mac0 >> 24;
    info->mac[3] = mac0 >> 16;
    info->mac[4] = mac0 >> 8;
    info->mac[5] = mac0;
}

int soc_temperature(void)
{
    uint32_t t;

    if (!(rd32(TMU_BASE + TMU_TER) & TMU_TER_EN)) {
        wr32(TMU_BASE + TMU_TER, TMU_TER_EN);
    }
    t = rd32(TMU_BASE + TMU_TRITSR) & 0xff;
    /* Linux treats values below 10 C as a measurement still running */
    return t < 10 ? -1 : (int)t;
}
