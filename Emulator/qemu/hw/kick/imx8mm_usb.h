/*
 * i.MX 8M Mini USB OTG controller (host role) with its usbmisc block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_IMX8MM_USB_H
#define HW_KICK_IMX8MM_USB_H

#include "hw/usb/chipidea.h"
#include "qom/object.h"

#define TYPE_IMX8MM_USB "imx8mm-usb"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMUSBState, IMX8MM_USB)

/* usbmisc ("non-core") registers follow the controller at +0x200 */
#define IMX8MM_USBMISC_OFFSET   0x200
#define IMX8MM_USBMISC_SIZE     0x200

struct IMX8MMUSBState {
    ChipideaState parent_obj;

    MemoryRegion portsc;
    MemoryRegion usbmisc;
    uint32_t misc[IMX8MM_USBMISC_SIZE / 4];
};

#endif
