/*
 * i.MX 8M Mini DDR controller (Synopsys uMCTL2) and DDR PHY (PUB)
 *
 * Enough of the DDR subsystem for U-Boot SPL's LPDDR4 bring-up
 * (drivers/ddr/imx/imx8m/ddr_init.c, ddrphy_utils.c, helper.c) to run to
 * completion, as it does on hardware. The memory itself is QEMU RAM and
 * does not depend on these registers; what is modelled is the control
 * flow SPL and the kernel's DDR frequency code wait for:
 *
 * Controller (0x3d400000), register file 0x0000..0x3fff:
 *   STAT 0x004      operating_mode [2:0]: 0 init, 1 normal (once
 *                   DFIMISC.dfi_init_complete_en is set), 3 self refresh
 *                   (while PWRCTL.selfref_sw), selfref_type [5:4]
 *   MRSTAT 0x018    mode register access never busy
 *   DFIMISC 0x1b0   dfi_init_start [5] completes PHY init at once:
 *   DFISTAT 0x1bc   dfi_init_complete [0] latches and stays set
 *   DBGCAM 0x308    command and data queues always empty
 *   SWCTL/SWSTAT    0x320/0x324: sw_done_ack follows sw_done
 *   SBRCTL/SBRSTAT  0xf24/0xf28: the scrubber finishes at once
 *
 * PHY (0x3c000000), 32-bit APB words at byte offset 4 * address:
 *   Every location is storage, so the training firmware images SPL loads
 *   into IMEM/DMEM read back as written (SPL verifies them).
 *   MicroReset (0xd0099): the 0x1 -> 0x0 transition starts the training
 *   firmware. Training is not simulated: the firmware result is a single
 *   "training complete, pass" (0x07) message in the PMU mailbox.
 *   Mailbox: UctShadowRegs (0xd0004) bit 0 reads 0 while a message is
 *   pending, UctWriteOnlyShadow/UctDatWriteOnlyShadow (0xd0032/0xd0034)
 *   hold it, DctWriteProt (0xd0031) = 0 acknowledges, = 1 re-arms.
 *   CalBusy (0x20097) reads 0: calibration is idle.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"

#include "imx8mm_ddrc.h"

#define DDRC_STAT           0x004
#define DDRC_MRSTAT         0x018
#define DDRC_PWRCTL         0x030
#define DDRC_DFIMISC        0x1b0
#define DDRC_DFISTAT        0x1bc
#define DDRC_DBGCAM         0x308
#define DDRC_SWCTL          0x320
#define DDRC_SWSTAT         0x324
#define DDRC_SBRCTL         0xf24
#define DDRC_SBRSTAT        0xf28

#define PWRCTL_SELFREF_SW   BIT(5)
#define DFIMISC_COMPLETE_EN BIT(0)
#define DFIMISC_INIT_START  BIT(5)
#define SBRCTL_SCRUB_EN     BIT(0)
#define SBRSTAT_DONE        BIT(1)

/* dbg_rd_q_empty, dbg_wr_q_empty, rd/wr_data_pipeline_empty */
#define DBGCAM_IDLE         (BIT(25) | BIT(26) | BIT(28) | BIT(29))

/* PHY word addresses */
#define PHY_UCT_SHADOW      0xd0004
#define PHY_DCT_WRITE_PROT  0xd0031
#define PHY_UCT_WRITE_ONLY  0xd0032
#define PHY_UCT_DAT_WO      0xd0034
#define PHY_MICRO_RESET     0xd0099
#define PHY_CAL_BUSY        0x20097

#define MAIL_TRAINING_PASS  0x07

#define R(off)              ((off) / 4)

static uint64_t imx8mm_ddrc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMDDRCState *s = IMX8MM_DDRC(opaque);
    uint32_t pwrctl, dfimisc;

    if (offset >= IMX8MM_DDRC_NUM_REGS * 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", TYPE_IMX8MM_DDRC, offset);
        return 0;
    }

    switch (offset) {
    case DDRC_STAT:
        dfimisc = s->regs[R(DDRC_DFIMISC)];
        pwrctl = s->regs[R(DDRC_PWRCTL)];
        if (!(dfimisc & DFIMISC_COMPLETE_EN)) {
            return 0;                           /* init */
        }
        if (pwrctl & PWRCTL_SELFREF_SW) {
            return 0x3 | (0x2 << 4);            /* software self refresh */
        }
        return 0x1;                             /* normal */
    case DDRC_MRSTAT:
        return 0;
    case DDRC_DFISTAT:
        return s->dfi_init_complete ? 1 : 0;
    case DDRC_DBGCAM:
        return DBGCAM_IDLE;
    case DDRC_SWSTAT:
        return s->regs[R(DDRC_SWCTL)] & 1;
    case DDRC_SBRSTAT:
        return (s->regs[R(DDRC_SBRCTL)] & SBRCTL_SCRUB_EN) ? SBRSTAT_DONE : 0;
    default:
        return s->regs[R(offset)];
    }
}

static void imx8mm_ddrc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    IMX8MMDDRCState *s = IMX8MM_DDRC(opaque);

    if (offset >= IMX8MM_DDRC_NUM_REGS * 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_DDRC, offset);
        return;
    }
    if (offset == DDRC_DFIMISC && (value & DFIMISC_INIT_START)) {
        /* The PHY finishes its DFI initialisation immediately */
        s->dfi_init_complete = true;
    }
    s->regs[R(offset)] = value;
}

static void imx8mm_ddrphy_post_mail(IMX8MMDDRCState *s, uint32_t mail)
{
    if (s->mail_count < IMX8MM_DDRPHY_MAIL_MAX) {
        s->mail[s->mail_count++] = mail;
    }
    if (!s->mail_valid && s->mail_count) {
        s->mail_valid = true;
    }
}

static uint32_t imx8mm_ddrphy_get(IMX8MMDDRCState *s, uint32_t addr)
{
    return GPOINTER_TO_UINT(g_hash_table_lookup(s->phy,
                                                GUINT_TO_POINTER(addr)));
}

static uint64_t imx8mm_ddrphy_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    IMX8MMDDRCState *s = IMX8MM_DDRC(opaque);
    uint32_t addr = offset / 4;

    switch (addr) {
    case PHY_UCT_SHADOW:
        /* bit 0 low: a message is waiting for the host */
        return s->mail_valid ? 0 : 1;
    case PHY_UCT_WRITE_ONLY:
        return s->mail_count ? s->mail[0] & 0xffff : 0;
    case PHY_UCT_DAT_WO:
        return s->mail_count ? s->mail[0] >> 16 : 0;
    case PHY_CAL_BUSY:
        return 0;
    default:
        return imx8mm_ddrphy_get(s, addr);
    }
}

static void imx8mm_ddrphy_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    IMX8MMDDRCState *s = IMX8MM_DDRC(opaque);
    uint32_t addr = offset / 4;
    uint32_t old = imx8mm_ddrphy_get(s, addr);

    g_hash_table_insert(s->phy, GUINT_TO_POINTER(addr),
                        GUINT_TO_POINTER((uint32_t)value));

    switch (addr) {
    case PHY_DCT_WRITE_PROT:
        if (value == 0 && s->mail_valid) {
            /* Host acknowledged: the PMU drops the message */
            s->mail_valid = false;
            memmove(s->mail, s->mail + 1,
                    sizeof(s->mail[0]) * (IMX8MM_DDRPHY_MAIL_MAX - 1));
            s->mail_count--;
        } else if (value == 1 && s->mail_count) {
            /* Re-armed: the next message becomes visible */
            s->mail_valid = true;
        }
        break;
    case PHY_MICRO_RESET:
        if ((old & 1) && !(value & 1)) {
            /* Training firmware runs and reports success */
            s->micro_running = true;
            s->trainings++;
            qemu_log_mask(LOG_UNIMP, "%s: DDR training run %u: training is "
                          "not simulated, reporting pass\n",
                          TYPE_IMX8MM_DDRC, s->trainings);
            imx8mm_ddrphy_post_mail(s, MAIL_TRAINING_PASS);
        } else if (value & 1) {
            s->micro_running = false;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx8mm_ddrc_ops = {
    .read = imx8mm_ddrc_read,
    .write = imx8mm_ddrc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * PHY registers are 16 bits wide on a 32-bit stride; SPL writes the
 * training firmware with 16-bit stores.
 */
static const MemoryRegionOps imx8mm_ddrphy_ops = {
    .read = imx8mm_ddrphy_read,
    .write = imx8mm_ddrphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx8mm_ddrc_reset(DeviceState *dev)
{
    IMX8MMDDRCState *s = IMX8MM_DDRC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->dfi_init_complete = false;
    g_hash_table_remove_all(s->phy);
    s->mail_count = 0;
    s->mail_valid = false;
    s->micro_running = false;
}

static void imx8mm_ddrc_init(Object *obj)
{
    IMX8MMDDRCState *s = IMX8MM_DDRC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem_ctl, obj, &imx8mm_ddrc_ops, s,
                          TYPE_IMX8MM_DDRC, IMX8MM_DDRC_MMIO_SIZE);
    memory_region_init_io(&s->iomem_phy, obj, &imx8mm_ddrphy_ops, s,
                          TYPE_IMX8MM_DDRC ".phy", IMX8MM_DDRPHY_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem_ctl);
    sysbus_init_mmio(sbd, &s->iomem_phy);
    s->phy = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void imx8mm_ddrc_finalize(Object *obj)
{
    g_hash_table_destroy(IMX8MM_DDRC(obj)->phy);
}

static void imx8mm_ddrc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_ddrc_reset);
    dc->desc = "i.MX8MM DDR controller and PHY";
    /* The PHY register file lives in a hash table; not migratable */
    dc->vmsd = NULL;
}

static const TypeInfo imx8mm_ddrc_types[] = {
    {
        .name              = TYPE_IMX8MM_DDRC,
        .parent            = TYPE_SYS_BUS_DEVICE,
        .instance_size     = sizeof(IMX8MMDDRCState),
        .instance_init     = imx8mm_ddrc_init,
        .instance_finalize = imx8mm_ddrc_finalize,
        .class_init        = imx8mm_ddrc_class_init,
    },
};

DEFINE_TYPES(imx8mm_ddrc_types)
