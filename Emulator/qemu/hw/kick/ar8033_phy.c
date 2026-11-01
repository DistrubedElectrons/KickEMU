/*
 * Qualcomm Atheros AR8033 Gigabit Ethernet PHY on the i.MX ENET MDIO bus
 *
 * The VAR-SOM-MX8M-MINI connects ENET1 in RGMII mode to an AR8033 at MDIO
 * address 4 (imx8mm-var-som.dtsi, "at803x,*" properties). The upstream
 * imx_fec model has a built-in LAN9118-style PHY with a fixed ID, so the
 * board disables it and this device answers the MDIO frames instead: it
 * lays a 4-byte window over the ENET MMFR register (offset 0x40), decodes
 * clause 22 frames, and forwards every write to the MAC so the MII
 * completion interrupt (EIR.MII) behaves exactly as before.
 *
 * Registers (AR8033 datasheet):
 *   0x00-0x0f IEEE clause 22 (ID 0x004d/0xd074)
 *   0x0d/0x0e MMD access control/data (clause 45 over clause 22)
 *   0x11      PHY specific status: speed [15:14], duplex [13],
 *             resolved [11], link [10]
 *   0x12/0x13 interrupt enable / status (status clears on read)
 *   0x1d/0x1e debug port address / data
 *
 * Link state follows the QEMU NIC link (set_link). Autonegotiation always
 * resolves to 1000BASE-T full duplex against an ideal partner.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/reset.h"

#include "ar8033_phy.h"

#define ENET_MMFR_OFFSET    0x40

/* MMFR fields (i.MX ENET MII Management Frame Register) */
#define MMFR_ST(v)          extract32((v), 30, 2)
#define MMFR_OP(v)          extract32((v), 28, 2)
#define MMFR_PA(v)          extract32((v), 23, 5)
#define MMFR_RA(v)          extract32((v), 18, 5)
#define MMFR_DATA(v)        extract32((v), 0, 16)
#define MMFR_OP_WRITE       1
#define MMFR_OP_READ        2

#define MII_BMCR            0x00
#define MII_BMSR            0x01
#define MII_PHYID1          0x02
#define MII_PHYID2          0x03
#define MII_ANAR            0x04
#define MII_ANLPAR          0x05
#define MII_ANER            0x06
#define MII_CTRL1000        0x09
#define MII_STAT1000        0x0a
#define MII_MMD_CTRL        0x0d
#define MII_MMD_DATA        0x0e
#define MII_ESTATUS         0x0f
#define AT803X_SPEC_STATUS  0x11
#define AT803X_INT_ENABLE   0x12
#define AT803X_INT_STATUS   0x13
#define AT803X_DEBUG_ADDR   0x1d
#define AT803X_DEBUG_DATA   0x1e

#define BMCR_RESET          BIT(15)
#define BMCR_ANRESTART      BIT(9)
#define BMSR_BASE           0x7949  /* 10/100 HD/FD, ext status, AN able */
#define BMSR_LSTATUS        BIT(2)
#define BMSR_ANEGCOMPLETE   BIT(5)

#define SPEC_STATUS_1000FD  0xa800  /* 1000 Mb/s, full duplex, resolved */
#define SPEC_STATUS_LINK    BIT(10)

#define INT_LINK_FAIL       BIT(11)
#define INT_LINK_UP         BIT(10)

#define MMD_FUNC_ADDR       0

static void ar8033_reset_regs(AR8033PhyState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->debug, 0, sizeof(s->debug));
    s->regs[MII_BMCR] = 0x1140;         /* AN enabled, full duplex, 1000 */
    s->regs[MII_PHYID1] = 0x004d;
    s->regs[MII_PHYID2] = 0xd074;
    s->regs[MII_ANAR] = 0x1de1;
    s->regs[MII_CTRL1000] = 0x0300;
    s->regs[MII_ESTATUS] = 0x3000;      /* 1000BASE-T FD and HD */
    s->regs[0x10] = 0x0862;             /* function control */
    s->debug[0x00] = 0x82ee;            /* RGMII RX clock delay enabled */
    s->debug[0x05] = 0x3c47;            /* RGMII TX clock delay */
    s->debug_addr = 0;
    s->mmd_ctrl = 0;
    s->mmd_addr = 0;
    g_hash_table_remove_all(s->mmd);
}

static void ar8033_update_link(AR8033PhyState *s)
{
    bool up = s->enet->nic && !qemu_get_queue(s->enet->nic)->link_down;

    if (up != s->link_up) {
        s->regs[AT803X_INT_STATUS] |= up ? INT_LINK_UP : INT_LINK_FAIL;
        s->link_up = up;
    }
}

static uint16_t ar8033_mmd_read(AR8033PhyState *s)
{
    uint32_t key = (s->mmd_ctrl & 0x1f) << 16 | s->mmd_addr;

    return GPOINTER_TO_UINT(g_hash_table_lookup(s->mmd,
                                                GUINT_TO_POINTER(key)));
}

static void ar8033_mmd_write(AR8033PhyState *s, uint16_t value)
{
    uint32_t key = (s->mmd_ctrl & 0x1f) << 16 | s->mmd_addr;

    g_hash_table_insert(s->mmd, GUINT_TO_POINTER(key),
                        GUINT_TO_POINTER((uint32_t)value));
}

static uint16_t ar8033_read(AR8033PhyState *s, int reg)
{
    uint16_t v;

    ar8033_update_link(s);
    switch (reg) {
    case MII_BMSR:
        return BMSR_BASE | (s->link_up ? BMSR_LSTATUS | BMSR_ANEGCOMPLETE : 0);
    case MII_ANLPAR:
        return s->link_up ? 0xcde1 : 0;     /* partner: all modes, ACK */
    case MII_ANER:
        return s->link_up ? 0x0001 : 0;     /* partner AN able */
    case MII_STAT1000:
        return s->link_up ? 0x3c00 : 0;     /* partner 1000 FD/HD */
    case AT803X_SPEC_STATUS:
        return s->link_up ? SPEC_STATUS_1000FD | SPEC_STATUS_LINK : 0;
    case AT803X_INT_STATUS:
        v = s->regs[reg];
        s->regs[reg] = 0;                   /* clear on read */
        return v;
    case MII_MMD_DATA:
        if ((s->mmd_ctrl >> 14) == MMD_FUNC_ADDR) {
            return s->mmd_addr;
        }
        return ar8033_mmd_read(s);
    case AT803X_DEBUG_DATA:
        return s->debug[s->debug_addr & 0x3f];
    default:
        return s->regs[reg];
    }
}

static void ar8033_write(AR8033PhyState *s, int reg, uint16_t value)
{
    switch (reg) {
    case MII_BMCR:
        if (value & BMCR_RESET) {
            ar8033_reset_regs(s);
            return;
        }
        /* Restart autonegotiation completes at once (self-clearing) */
        s->regs[reg] = value & ~BMCR_ANRESTART;
        break;
    case MII_BMSR:
    case MII_PHYID1:
    case MII_PHYID2:
    case MII_ANLPAR:
    case MII_ANER:
    case MII_STAT1000:
    case MII_ESTATUS:
    case AT803X_SPEC_STATUS:
    case AT803X_INT_STATUS:
        break;  /* read-only */
    case MII_MMD_CTRL:
        s->mmd_ctrl = value;
        break;
    case MII_MMD_DATA:
        if ((s->mmd_ctrl >> 14) == MMD_FUNC_ADDR) {
            s->mmd_addr = value;
        } else {
            ar8033_mmd_write(s, value);
        }
        break;
    case AT803X_DEBUG_ADDR:
        s->debug_addr = value;
        break;
    case AT803X_DEBUG_DATA:
        s->debug[s->debug_addr & 0x3f] = value;
        break;
    default:
        s->regs[reg] = value;
        break;
    }
}

static uint64_t ar8033_mmfr_read(void *opaque, hwaddr offset, unsigned size)
{
    AR8033PhyState *s = AR8033_PHY(opaque);

    return s->mmfr_value;
}

static void ar8033_mmfr_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    AR8033PhyState *s = AR8033_PHY(opaque);
    uint32_t frame = value;

    s->mmfr_value = frame;
    if (MMFR_ST(frame) == 1 && MMFR_PA(frame) == s->phy_addr) {
        if (MMFR_OP(frame) == MMFR_OP_READ) {
            s->mmfr_value = deposit32(frame, 0, 16,
                                      ar8033_read(s, MMFR_RA(frame)));
        } else if (MMFR_OP(frame) == MMFR_OP_WRITE) {
            ar8033_write(s, MMFR_RA(frame), MMFR_DATA(frame));
        }
    } else if (MMFR_OP(frame) == MMFR_OP_READ) {
        /* No device at that address: the bus floats high */
        s->mmfr_value = deposit32(frame, 0, 16, 0xffff);
    }

    /* Let the MAC complete the management frame and raise EIR.MII */
    memory_region_dispatch_write(&s->enet->iomem, ENET_MMFR_OFFSET, value,
                                 MO_32, MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps ar8033_mmfr_ops = {
    .read = ar8033_mmfr_read,
    .write = ar8033_mmfr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ar8033_realize(DeviceState *dev, Error **errp)
{
    AR8033PhyState *s = AR8033_PHY(dev);

    if (!s->enet) {
        error_setg(errp, "%s: 'enet' link not set", TYPE_AR8033_PHY);
        return;
    }
    if (s->phy_addr > 31) {
        error_setg(errp, "%s: MDIO address %u out of range", TYPE_AR8033_PHY,
                   s->phy_addr);
        return;
    }
    memory_region_init_io(&s->mmfr, OBJECT(dev), &ar8033_mmfr_ops, s,
                          "ar8033.mmfr", 4);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        s->enet_base + ENET_MMFR_OFFSET,
                                        &s->mmfr, 1);
    /* Not on a bus: take part in system reset explicitly */
    qemu_register_resettable(OBJECT(dev));
}

static void ar8033_reset(DeviceState *dev)
{
    AR8033PhyState *s = AR8033_PHY(dev);

    ar8033_reset_regs(s);
    s->mmfr_value = 0;
    s->link_up = false;
}

static void ar8033_init(Object *obj)
{
    AR8033PhyState *s = AR8033_PHY(obj);

    s->mmd = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void ar8033_finalize(Object *obj)
{
    AR8033PhyState *s = AR8033_PHY(obj);

    g_hash_table_destroy(s->mmd);
}

static const Property ar8033_properties[] = {
    DEFINE_PROP_LINK("enet", AR8033PhyState, enet, TYPE_IMX_FEC,
                     IMXFECState *),
    DEFINE_PROP_UINT64("enet-base", AR8033PhyState, enet_base, 0),
    DEFINE_PROP_UINT32("phy-addr", AR8033PhyState, phy_addr, 4),
};

static void ar8033_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ar8033_reset);
    device_class_set_props(dc, ar8033_properties);
    dc->realize = ar8033_realize;
    dc->desc = "Atheros AR8033 Ethernet PHY (ENET MDIO)";
    dc->user_creatable = false;
}

static const TypeInfo ar8033_types[] = {
    {
        .name              = TYPE_AR8033_PHY,
        .parent            = TYPE_DEVICE,
        .instance_size     = sizeof(AR8033PhyState),
        .instance_init     = ar8033_init,
        .instance_finalize = ar8033_finalize,
        .class_init        = ar8033_class_init,
    },
};

DEFINE_TYPES(ar8033_types)
