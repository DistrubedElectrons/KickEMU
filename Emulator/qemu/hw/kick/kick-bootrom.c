/*
 * i.MX 8M Mini BootROM emulation (boot=rom)
 *
 * Replaces the mask ROM for the SD/eMMC boot path (IMX8MMRM chapter 6.1
 * "System Boot"): it reads the Image Vector Table at 33 KiB, copies the
 * image described by its boot data to on-chip memory (SPL is linked at
 * 0x7e1000, so it spans ITCM and DTCM), records the boot device in the ROM
 * software info structure U-Boot reads (pointer at 0x9e8,
 * arch/arm/mach-imx/cpu.c get_boot_device) and starts CPU 0 at the IVT
 * entry point in EL3.
 *
 * IVT (HAB v4):  tag 0xd1, length 0x0020 (big endian), version 0x4x;
 *                entry, reserved, dcd, boot_data, self, csf, reserved
 * Boot data:     start, size, plugin
 *
 * Not emulated: DCD processing, HAB signature checks (CSF), plugin images,
 * USB serial download, FlexSPI/NAND boot and the ROM API. Those are
 * reported and refused or skipped as noted below.
 *
 * The image is registered as a ROM blob so a guest reset reloads it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "system/block-backend.h"
#include "target/arm/cpu.h"

#include "kick-bootrom.h"

#define BOOTROM_IVT_KIB         33
#define BOOTROM_IVT_OFFSET      (BOOTROM_IVT_KIB * KiB)
#define IVT_TAG                 0xd1
#define IVT_LENGTH              0x20
#define IVT_VERSION_MASK        0xf0
#define IVT_VERSION_HAB4        0x40
/* Largest image the ROM can place: ITCM + DTCM + OCRAM */
#define BOOTROM_MAX_IMAGE       (512 * KiB)

/* ROM software info: pointer at 0x9e8, structure placed in the ROM data */
#define ROM_SW_INFO_PTR         0x9e8
#define ROM_SW_INFO_ADDR        0xa00

typedef struct QEMU_PACKED IMX8MIvt {
    uint8_t tag;
    uint16_t length;        /* big endian */
    uint8_t version;
    uint32_t entry;
    uint32_t reserved1;
    uint32_t dcd;
    uint32_t boot_data;
    uint32_t self;
    uint32_t csf;
    uint32_t reserved2;
} IMX8MIvt;

typedef struct QEMU_PACKED IMX8MBootData {
    uint32_t start;
    uint32_t size;
    uint32_t plugin;
} IMX8MBootData;

/* struct bootrom_sw_info in U-Boot arch-imx8m/imx-regs.h */
typedef struct QEMU_PACKED IMX8MRomSwInfo {
    uint8_t reserved1;
    uint8_t boot_dev_instance;
    uint8_t boot_dev_type;
    uint8_t reserved2;
    uint32_t core_freq;
    uint32_t axi_freq;
    uint32_t ddr_freq;
    uint32_t tick_freq;
    uint32_t reserved3[3];
} IMX8MRomSwInfo;

/* The ROM can only load into on-chip RAM */
static bool kick_bootrom_target_ok(uint32_t start, uint32_t size)
{
    static const int regions[] = {
        FSL_IMX8MM_TCM_ITCM, FSL_IMX8MM_OCRAM,
    };
    uint64_t end = (uint64_t)start + size;

    for (int i = 0; i < ARRAY_SIZE(regions); i++) {
        const FslImx8mmMemMapEntry *m = &fsl_imx8mm_memmap[regions[i]];
        /* ITCM and DTCM are contiguous and form one window */
        hwaddr limit = regions[i] == FSL_IMX8MM_TCM_ITCM ?
            fsl_imx8mm_memmap[FSL_IMX8MM_TCM_DTCM].addr +
            fsl_imx8mm_memmap[FSL_IMX8MM_TCM_DTCM].size :
            m->addr + m->size;

        if (start >= m->addr && end <= limit) {
            return true;
        }
    }
    return false;
}

bool kick_bootrom_setup(FslImx8mmState *soc, BlockBackend *blk,
                        KickBootDevType type, int instance, Error **errp)
{
    IMX8MIvt ivt;
    IMX8MBootData bd;
    IMX8MRomSwInfo info = { 0 };
    uint32_t sw_info_ptr = cpu_to_le32(ROM_SW_INFO_ADDR);
    g_autofree uint8_t *image = NULL;
    int64_t disk_off;
    uint32_t entry, start, size, self;

    if (blk_pread(blk, BOOTROM_IVT_OFFSET, sizeof(ivt), &ivt, 0) < 0) {
        error_setg(errp, "bootrom: cannot read the IVT at %d KiB",
                   BOOTROM_IVT_KIB);
        return false;
    }
    if (ivt.tag != IVT_TAG || be16_to_cpu(ivt.length) != IVT_LENGTH ||
        (ivt.version & IVT_VERSION_MASK) != IVT_VERSION_HAB4) {
        error_setg(errp, "bootrom: no i.MX8M IVT at %d KiB (tag 0x%02x)",
                   BOOTROM_IVT_KIB, ivt.tag);
        return false;
    }

    entry = le32_to_cpu(ivt.entry);
    self = le32_to_cpu(ivt.self);
    if (ivt.dcd) {
        warn_report("bootrom: DCD at 0x%x is not processed",
                    le32_to_cpu(ivt.dcd));
    }
    if (ivt.csf) {
        warn_report("bootrom: CSF present, HAB authentication is not "
                    "emulated (open part behaviour)");
    }

    /* Boot data lives inside the image: locate it relative to the IVT */
    disk_off = BOOTROM_IVT_OFFSET + (int64_t)le32_to_cpu(ivt.boot_data) - self;
    if (disk_off < 0 ||
        blk_pread(blk, disk_off, sizeof(bd), &bd, 0) < 0) {
        error_setg(errp, "bootrom: cannot read the boot data");
        return false;
    }
    start = le32_to_cpu(bd.start);
    size = le32_to_cpu(bd.size);
    if (le32_to_cpu(bd.plugin)) {
        error_setg(errp, "bootrom: plugin images are not supported");
        return false;
    }
    if (size == 0 || size > BOOTROM_MAX_IMAGE ||
        !kick_bootrom_target_ok(start, size)) {
        error_setg(errp, "bootrom: image 0x%x+0x%x is outside on-chip RAM",
                   start, size);
        return false;
    }

    /* Image byte at address A is at disk offset IVT + (A - self) */
    disk_off = BOOTROM_IVT_OFFSET + (int64_t)start - self;
    image = g_malloc(size);
    if (disk_off < 0 || blk_pread(blk, disk_off, size, image, 0) < 0) {
        error_setg(errp, "bootrom: cannot read %u bytes of boot image", size);
        return false;
    }
    rom_add_blob_fixed("imx8mm.boot-image", image, size, start);

    /* Boot device record for SPL; frequencies are not reported */
    info.boot_dev_type = type;
    info.boot_dev_instance = instance;
    rom_add_blob_fixed("imx8mm.rom-sw-info", &info, sizeof(info),
                       ROM_SW_INFO_ADDR);
    rom_add_blob_fixed("imx8mm.rom-sw-info-ptr", &sw_info_ptr,
                       sizeof(sw_info_ptr), ROM_SW_INFO_PTR);

    /* CPU 0 leaves reset at the IVT entry in EL3 */
    object_property_set_uint(OBJECT(&soc->cpu[0]), "rvbar", entry,
                             &error_abort);

    info_report("bootrom: %s%d, image 0x%x+0x%x, entry 0x%x",
                type == KICK_BOOTDEV_MMC ? "MMC" : "SD", instance + 1,
                start, size, entry);
    return true;
}
