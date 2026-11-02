/*
 * i.MX 8M Mini USB OTG controller (host role) with its usbmisc block
 *
 * Each of the two USB controllers (USB1 0x32e40000, USB2 0x32e50000) is a
 * ChipIdea HDRC whose host part is an EHCI with an integrated transaction
 * translator, followed by the "usbmisc" (non-core) registers at +0x200
 * (imx8mm.dtsi: "fsl,imx8mm-usb", "fsl,imx7d-usb" and "fsl,imx8mm-usbmisc",
 * "fsl,imx7d-usbmisc"). The 5.4 kernel binds ci_hdrc_imx with the imx7d
 * flags and usbmisc_imx's imx7d ops; the PHY is a "usb-nop-xceiv" without
 * registers.
 *
 * QEMU's usb-chipidea gives the EHCI register layout (capabilities at
 * 0x100, operational registers at 0x140, PORTSC1 at 0x184) and reports
 * host capability only, so ci_hdrc runs the controller in host role (the
 * gadget role fails with -ENXIO). Two things are added here:
 *
 * - The root port of a ChipIdea EHCI has an integrated transaction
 *   translator (Linux: hcd->has_tt, ehci_is_TDI): full- and low-speed
 *   devices are enabled by the port reset like high-speed ones, and
 *   PORTSC1.PSPD (bits 27:26, 0 full, 1 low, 2 high) reports the speed,
 *   which ehci_port_speed() reads instead of handing the port to a
 *   companion. Generic EHCI only enables high-speed devices and leaves
 *   PSPD 0, so Linux would treat every device as full speed and refuse
 *   full-speed ones ("Failed to enable port"). QEMU's EHCI executes
 *   transfers without split transactions, so no TT behaviour is needed
 *   beyond the port. Isochronous full-speed transfers (siTD) are not
 *   implemented by QEMU's EHCI.
 * - usbmisc: USBNC_n_CTRL1 (+0x00) resets to 0x30001000, the value
 *   usbmisc_imx7d_power_lost_check() compares against; CTRL2, PHY_CFG1/2
 *   and the rest are storage. PHY_STATUS (+0x3c) reads 0: no VBUS and no
 *   charger, which only matters for the gadget role.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/usb/hcd-ehci.h"
#include "imx8mm_usb.h"

#define USB_PORTSC1             (0x140 + 0x44)
#define PORTSC_CONNECT          (1u << 0)
#define PORTSC_PED              (1u << 2)
#define PORTSC_PRESET           (1u << 8)
#define PORTSC_PSPD_SHIFT       26
#define PORTSC_PSPD_MASK        (3u << PORTSC_PSPD_SHIFT)
#define PSPD_FULL               0
#define PSPD_LOW                1
#define PSPD_HIGH               2

#define USBMISC_CTRL1           0x00
#define USBMISC_PHY_STATUS      0x3c
#define USBMISC_CTRL1_RESET     0x30001000

static EHCIState *imx8mm_usb_ehci(IMX8MMUSBState *s)
{
    return &SYS_BUS_EHCI(s)->ehci;
}

static uint64_t imx8mm_usb_portsc_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    IMX8MMUSBState *s = opaque;
    EHCIState *e = imx8mm_usb_ehci(s);
    USBDevice *dev = e->ports[0].dev;
    uint32_t v = e->mem_ports.ops->read(e, 0, 4);

    v &= ~PORTSC_PSPD_MASK;
    if ((v & PORTSC_CONNECT) && dev && dev->attached) {
        uint32_t pspd = dev->speed == USB_SPEED_LOW ? PSPD_LOW :
                        dev->speed == USB_SPEED_FULL ? PSPD_FULL : PSPD_HIGH;

        v |= pspd << PORTSC_PSPD_SHIFT;
    }
    return v;
}

static void imx8mm_usb_portsc_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    IMX8MMUSBState *s = opaque;
    EHCIState *e = imx8mm_usb_ehci(s);
    USBDevice *dev = e->ports[0].dev;
    bool reset_ends = (e->portsc[0] & PORTSC_PRESET) && !(val & PORTSC_PRESET);

    e->mem_ports.ops->write(e, 0, val, 4);
    /*
     * The integrated TT: a full- or low-speed device is enabled at the end
     * of the port reset too (generic EHCI does so for high speed only).
     */
    if (reset_ends && dev && dev->attached && dev->speed != USB_SPEED_HIGH &&
        (e->portsc[0] & PORTSC_CONNECT)) {
        e->portsc[0] |= PORTSC_PED;
    }
}

static const MemoryRegionOps imx8mm_usb_portsc_ops = {
    .read = imx8mm_usb_portsc_read,
    .write = imx8mm_usb_portsc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t imx8mm_usbmisc_read(void *opaque, hwaddr addr, unsigned size)
{
    IMX8MMUSBState *s = opaque;

    return addr == USBMISC_PHY_STATUS ? 0 : s->misc[addr / 4];
}

static void imx8mm_usbmisc_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    IMX8MMUSBState *s = opaque;

    if (addr != USBMISC_PHY_STATUS) {
        s->misc[addr / 4] = val;
    }
}

static const MemoryRegionOps imx8mm_usbmisc_ops = {
    .read = imx8mm_usbmisc_read,
    .write = imx8mm_usbmisc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx8mm_usb_init(Object *obj)
{
    IMX8MMUSBState *s = IMX8MM_USB(obj);

    memory_region_init_io(&s->usbmisc, obj, &imx8mm_usbmisc_ops, s,
                          TYPE_IMX8MM_USB ".usbmisc", IMX8MM_USBMISC_SIZE);
    /* MMIO 1: usbmisc at +0x200 (MMIO 0 is the EHCI/ChipIdea window) */
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->usbmisc);
}

static DeviceRealize imx8mm_usb_parent_realize;
static ResettablePhases imx8mm_usb_parent_phases;

static void imx8mm_usb_realize(DeviceState *dev, Error **errp)
{
    IMX8MMUSBState *s = IMX8MM_USB(dev);
    EHCIState *e = imx8mm_usb_ehci(s);
    Error *local_err = NULL;

    imx8mm_usb_parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    /* The integrated TT takes full- and low-speed devices on the root port */
    e->ports[0].speedmask |= USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL;
    memory_region_init_io(&s->portsc, OBJECT(dev), &imx8mm_usb_portsc_ops, s,
                          TYPE_IMX8MM_USB ".portsc1", 4);
    memory_region_add_subregion_overlap(&e->mem, USB_PORTSC1, &s->portsc, 1);
}

static void imx8mm_usb_reset_hold(Object *obj, ResetType type)
{
    IMX8MMUSBState *s = IMX8MM_USB(obj);

    if (imx8mm_usb_parent_phases.hold) {
        imx8mm_usb_parent_phases.hold(obj, type);
    }
    memset(s->misc, 0, sizeof(s->misc));
    s->misc[USBMISC_CTRL1 / 4] = USBMISC_CTRL1_RESET;
}

static void imx8mm_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    imx8mm_usb_parent_realize = dc->realize;
    dc->realize = imx8mm_usb_realize;
    resettable_class_set_parent_phases(RESETTABLE_CLASS(klass), NULL,
                                       imx8mm_usb_reset_hold, NULL,
                                       &imx8mm_usb_parent_phases);
    dc->desc = "i.MX8MM USB controller (host) with usbmisc";
}

static const TypeInfo imx8mm_usb_types[] = {
    {
        .name          = TYPE_IMX8MM_USB,
        .parent        = TYPE_CHIPIDEA,
        .instance_size = sizeof(IMX8MMUSBState),
        .instance_init = imx8mm_usb_init,
        .class_init    = imx8mm_usb_class_init,
    },
};

DEFINE_TYPES(imx8mm_usb_types)
