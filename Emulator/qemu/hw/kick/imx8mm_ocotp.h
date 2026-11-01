/*
 * i.MX 8M Mini On-Chip OTP Controller (OCOTP)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_OCOTP_H
#define KICK_IMX8MM_OCOTP_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_IMX8MM_OCOTP "imx8mm.ocotp"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMOCOTPState, IMX8MM_OCOTP)

#define IMX8MM_OCOTP_MMIO_SIZE      0x10000
/* 64 banks of 4 words; shadow word n is at 0x400 + n * 0x10 */
#define IMX8MM_OCOTP_NUM_WORDS      256

struct IMX8MMOCOTPState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t ctrl;
    uint32_t timing;
    uint32_t data;
    uint32_t read_ctrl;
    uint32_t read_fuse_data;
    uint32_t sw_sticky;
    uint32_t scs;

    /* fuse: the programmed OTP array; shadow: what software reads */
    uint32_t fuse[IMX8MM_OCOTP_NUM_WORDS];
    uint32_t shadow[IMX8MM_OCOTP_NUM_WORDS];

    /* Properties */
    uint64_t unique_id;
    uint32_t tester3;
    uint32_t tester4;
    uint32_t boot_cfg[4];
    MACAddr mac;
    uint32_t *fuse_override;
    uint32_t fuse_override_len;
};

/*
 * Program the MAC0 fuses (factory programming by the board, e.g. with the
 * MAC address of the emulated NIC) and refresh the shadow registers.
 */
void imx8mm_ocotp_set_mac(IMX8MMOCOTPState *s, const MACAddr *mac);

#endif /* KICK_IMX8MM_OCOTP_H */
