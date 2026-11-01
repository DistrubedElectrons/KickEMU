/*
 * i.MX 8M Mini On-Chip OTP Controller (OCOTP)
 *
 * IMX8MMRM chapter 6.3 "On-Chip OTP Controller (OCOTP_CTRL)".
 *
 *   0x000 HW_OCOTP_CTRL (+SET/CLR/TOG)  ADDR [7:0], BUSY [8], ERROR [9],
 *                                       RELOAD_SHADOWS [10],
 *                                       WR_UNLOCK [31:16] (key 0x3e77)
 *   0x010 HW_OCOTP_TIMING
 *   0x020 HW_OCOTP_DATA                 write starts a fuse program cycle
 *   0x030 HW_OCOTP_READ_CTRL            READ_FUSE [0], self-clearing
 *   0x040 HW_OCOTP_READ_FUSE_DATA
 *   0x050 HW_OCOTP_SW_STICKY
 *   0x060 HW_OCOTP_SCS (+SET/CLR/TOG)
 *   0x400 + n*0x10                      shadow register of fuse word n
 *
 * Fuse words used by software:
 *   word 1/2 (0x410/0x420)  TESTER0/1: 64-bit unique ID (Linux
 *                           soc-imx8m.c /sys/devices/soc0/serial_number)
 *   word 4   (0x440)        TESTER3: speed grade / market segment
 *   word 5   (0x450)        TESTER4: part variant (U-Boot get_cpu_rev:
 *                           0 = i.MX8MM Quad)
 *   word 7.. (0x470..)      BOOT_CFG fuses
 *   word 36/37 (0x640/0x650) MAC address 0 (U-Boot imx_get_mac_from_fuse,
 *                           Linux nvmem cell with nvmem_macaddr_swap)
 *
 * Programming completes immediately; the new value lands in the fuse array
 * and becomes visible in the shadow registers after RELOAD_SHADOWS (or the
 * next reset), as on hardware. Fuse contents survive guest resets but are
 * not stored on the host.
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

#include "imx8mm_ocotp.h"

#define OCOTP_CTRL              0x000
#define OCOTP_TIMING            0x010
#define OCOTP_DATA              0x020
#define OCOTP_READ_CTRL         0x030
#define OCOTP_READ_FUSE_DATA    0x040
#define OCOTP_SW_STICKY         0x050
#define OCOTP_SCS               0x060
#define OCOTP_SHADOW_BASE       0x400
#define OCOTP_SHADOW_END        (OCOTP_SHADOW_BASE + \
                                 IMX8MM_OCOTP_NUM_WORDS * 0x10)

#define CTRL_ADDR_MASK          0x000000ff
#define CTRL_BUSY               BIT(8)
#define CTRL_ERROR              BIT(9)
#define CTRL_RELOAD_SHADOWS     BIT(10)
#define CTRL_WR_UNLOCK_MASK     0xffff0000
#define CTRL_WR_UNLOCK_KEY      0x3e770000
/* Bits software can write; BUSY is status */
#define CTRL_WRITABLE           (CTRL_ADDR_MASK | CTRL_ERROR | \
                                 CTRL_RELOAD_SHADOWS | CTRL_WR_UNLOCK_MASK)

#define READ_CTRL_READ_FUSE     BIT(0)

#define WORD_TESTER0            1
#define WORD_TESTER1            2
#define WORD_TESTER3            4
#define WORD_TESTER4            5
#define WORD_BOOT_CFG0          7
#define WORD_MAC0               36
#define WORD_MAC1               37

static uint32_t imx8mm_ocotp_bitop(uint32_t reg, hwaddr offset, uint32_t value)
{
    switch (offset & 0xc) {
    case 0x4:
        return reg | value;
    case 0x8:
        return reg & ~value;
    case 0xc:
        return reg ^ value;
    default:
        return value;
    }
}

static void imx8mm_ocotp_reload_shadows(IMX8MMOCOTPState *s)
{
    memcpy(s->shadow, s->fuse, sizeof(s->shadow));
}

/* A write to HW_OCOTP_DATA blows the bits that are 1 into word CTRL.ADDR */
static void imx8mm_ocotp_program(IMX8MMOCOTPState *s, uint32_t value)
{
    uint32_t addr = s->ctrl & CTRL_ADDR_MASK;

    if ((s->ctrl & CTRL_WR_UNLOCK_MASK) != CTRL_WR_UNLOCK_KEY ||
        (s->ctrl & (CTRL_BUSY | CTRL_ERROR))) {
        /* RM: a write without the unlock key or while busy sets ERROR */
        s->ctrl |= CTRL_ERROR;
        return;
    }

    s->fuse[addr] |= value;
    /* The unlock key is cleared by hardware after each program cycle */
    s->ctrl &= ~CTRL_WR_UNLOCK_MASK;
}

static uint64_t imx8mm_ocotp_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMOCOTPState *s = IMX8MM_OCOTP(opaque);

    if (offset >= OCOTP_SHADOW_BASE && offset < OCOTP_SHADOW_END) {
        if (offset & 0xc) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: unaligned shadow read 0x%"
                          HWADDR_PRIx "\n", TYPE_IMX8MM_OCOTP, offset);
            return 0;
        }
        return s->shadow[(offset - OCOTP_SHADOW_BASE) / 0x10];
    }

    switch (offset) {
    case OCOTP_CTRL ... OCOTP_CTRL + 0xc:
        return s->ctrl;
    case OCOTP_TIMING:
        return s->timing;
    case OCOTP_DATA:
        return s->data;
    case OCOTP_READ_CTRL:
        return s->read_ctrl;
    case OCOTP_READ_FUSE_DATA:
        return s->read_fuse_data;
    case OCOTP_SW_STICKY:
        return s->sw_sticky;
    case OCOTP_SCS ... OCOTP_SCS + 0xc:
        return s->scs;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_OCOTP, offset);
        return 0;
    }
}

static void imx8mm_ocotp_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX8MMOCOTPState *s = IMX8MM_OCOTP(opaque);
    uint32_t ctrl;

    if (offset >= OCOTP_SHADOW_BASE && offset < OCOTP_SHADOW_END) {
        /* Shadow registers can be overridden by software until reload */
        if (!(offset & 0xc)) {
            s->shadow[(offset - OCOTP_SHADOW_BASE) / 0x10] = value;
        }
        return;
    }

    switch (offset) {
    case OCOTP_CTRL ... OCOTP_CTRL + 0xc:
        ctrl = imx8mm_ocotp_bitop(s->ctrl, offset, value);
        s->ctrl = (s->ctrl & ~CTRL_WRITABLE) | (ctrl & CTRL_WRITABLE);
        if (s->ctrl & CTRL_RELOAD_SHADOWS) {
            /* Reload completes at once and the bit self-clears */
            imx8mm_ocotp_reload_shadows(s);
            s->ctrl &= ~CTRL_RELOAD_SHADOWS;
        }
        break;
    case OCOTP_TIMING:
        s->timing = value;
        break;
    case OCOTP_DATA:
        s->data = value;
        imx8mm_ocotp_program(s, value);
        break;
    case OCOTP_READ_CTRL:
        if (value & READ_CTRL_READ_FUSE) {
            /* Sense the fuse word directly, bypassing the shadow */
            s->read_fuse_data = s->fuse[s->ctrl & CTRL_ADDR_MASK];
        }
        s->read_ctrl = 0;
        break;
    case OCOTP_SW_STICKY:
        s->sw_sticky = value;
        break;
    case OCOTP_SCS ... OCOTP_SCS + 0xc:
        s->scs = imx8mm_ocotp_bitop(s->scs, offset, value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_OCOTP, offset);
        break;
    }
}

static const MemoryRegionOps imx8mm_ocotp_ops = {
    .read = imx8mm_ocotp_read,
    .write = imx8mm_ocotp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx8mm_ocotp_reset(DeviceState *dev)
{
    IMX8MMOCOTPState *s = IMX8MM_OCOTP(dev);

    s->ctrl = 0;
    s->timing = 0;
    s->data = 0;
    s->read_ctrl = 0;
    s->read_fuse_data = 0;
    s->sw_sticky = 0;
    s->scs = 0;
    /* The shadow registers are loaded from the fuses at every reset */
    imx8mm_ocotp_reload_shadows(s);
}

/* MAC0 fuse layout, see U-Boot imx_get_mac_from_fuse() for the 8M */
static void imx8mm_ocotp_fuse_mac(IMX8MMOCOTPState *s, const uint8_t *m)
{
    s->fuse[WORD_MAC0] = m[5] | m[4] << 8 | m[3] << 16 | (uint32_t)m[2] << 24;
    s->fuse[WORD_MAC1] = m[1] | m[0] << 8;
}

void imx8mm_ocotp_set_mac(IMX8MMOCOTPState *s, const MACAddr *mac)
{
    imx8mm_ocotp_fuse_mac(s, mac->a);
    imx8mm_ocotp_reload_shadows(s);
}

static void imx8mm_ocotp_realize(DeviceState *dev, Error **errp)
{
    IMX8MMOCOTPState *s = IMX8MM_OCOTP(dev);

    /* Factory fuse image from the properties */
    memset(s->fuse, 0, sizeof(s->fuse));
    s->fuse[WORD_TESTER0] = (uint32_t)s->unique_id;
    s->fuse[WORD_TESTER1] = (uint32_t)(s->unique_id >> 32);
    s->fuse[WORD_TESTER3] = s->tester3;
    s->fuse[WORD_TESTER4] = s->tester4;
    for (int i = 0; i < ARRAY_SIZE(s->boot_cfg); i++) {
        s->fuse[WORD_BOOT_CFG0 + i] = s->boot_cfg[i];
    }
    imx8mm_ocotp_fuse_mac(s, s->mac.a);

    /* Optional raw overrides: pairs of <word index, value> */
    if (s->fuse_override_len & 1) {
        error_setg(errp, "%s: fuse-override needs <word, value> pairs",
                   TYPE_IMX8MM_OCOTP);
        return;
    }
    for (uint32_t i = 0; i < s->fuse_override_len; i += 2) {
        uint32_t word = s->fuse_override[i];

        if (word >= IMX8MM_OCOTP_NUM_WORDS) {
            error_setg(errp, "%s: fuse word %u out of range",
                       TYPE_IMX8MM_OCOTP, word);
            return;
        }
        s->fuse[word] = s->fuse_override[i + 1];
    }
}

static void imx8mm_ocotp_init(Object *obj)
{
    IMX8MMOCOTPState *s = IMX8MM_OCOTP(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_ocotp_ops, s,
                          TYPE_IMX8MM_OCOTP, IMX8MM_OCOTP_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription imx8mm_ocotp_vmstate = {
    .name = TYPE_IMX8MM_OCOTP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, IMX8MMOCOTPState),
        VMSTATE_UINT32(timing, IMX8MMOCOTPState),
        VMSTATE_UINT32(data, IMX8MMOCOTPState),
        VMSTATE_UINT32(read_ctrl, IMX8MMOCOTPState),
        VMSTATE_UINT32(read_fuse_data, IMX8MMOCOTPState),
        VMSTATE_UINT32(sw_sticky, IMX8MMOCOTPState),
        VMSTATE_UINT32(scs, IMX8MMOCOTPState),
        VMSTATE_UINT32_ARRAY(fuse, IMX8MMOCOTPState, IMX8MM_OCOTP_NUM_WORDS),
        VMSTATE_UINT32_ARRAY(shadow, IMX8MMOCOTPState,
                             IMX8MM_OCOTP_NUM_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx8mm_ocotp_properties[] = {
    DEFINE_PROP_UINT64("unique-id", IMX8MMOCOTPState, unique_id, 0),
    DEFINE_PROP_UINT32("tester3", IMX8MMOCOTPState, tester3, 0),
    DEFINE_PROP_UINT32("tester4", IMX8MMOCOTPState, tester4, 0),
    DEFINE_PROP_UINT32("boot-cfg0", IMX8MMOCOTPState, boot_cfg[0], 0),
    DEFINE_PROP_UINT32("boot-cfg1", IMX8MMOCOTPState, boot_cfg[1], 0),
    DEFINE_PROP_UINT32("boot-cfg2", IMX8MMOCOTPState, boot_cfg[2], 0),
    DEFINE_PROP_UINT32("boot-cfg3", IMX8MMOCOTPState, boot_cfg[3], 0),
    DEFINE_PROP_MACADDR("mac-address", IMX8MMOCOTPState, mac),
    DEFINE_PROP_ARRAY("fuse-override", IMX8MMOCOTPState, fuse_override_len,
                      fuse_override, qdev_prop_uint32, uint32_t),
};

static void imx8mm_ocotp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_ocotp_reset);
    device_class_set_props(dc, imx8mm_ocotp_properties);
    dc->realize = imx8mm_ocotp_realize;
    dc->vmsd = &imx8mm_ocotp_vmstate;
    dc->desc = "i.MX8MM On-Chip OTP Controller";
}

static const TypeInfo imx8mm_ocotp_types[] = {
    {
        .name          = TYPE_IMX8MM_OCOTP,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMOCOTPState),
        .instance_init = imx8mm_ocotp_init,
        .class_init    = imx8mm_ocotp_class_init,
    },
};

DEFINE_TYPES(imx8mm_ocotp_types)
