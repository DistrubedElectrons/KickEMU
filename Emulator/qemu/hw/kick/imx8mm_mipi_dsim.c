/*
 * i.MX8M Mini MIPI DSI host controller (Samsung DSIM)
 *
 * Register model of the DSIM at 0x32e10000 (GIC SPI 18) as driven by the
 * vendor sec_mipi_dsim bridge driver (drivers/gpu/drm/bridge/sec-dsim.c and
 * drivers/gpu/drm/imx/sec_mipi_dsim-imx.c in varigit linux-imx 5.4).
 *
 * Modelled:
 *  - VERSION, STATUS (PLL stable, software reset release, clock and data
 *    lane stop states, HS clock ready), CLKCTRL, CONFIG, ESCMODE, the main
 *    and sub display timing registers, PHY timing and PLL registers;
 *  - the PLL: setting PLLCTRL.PLLEN locks after PLLTMR reference clock
 *    cycles and raises INTSRC.PLLSTABLE;
 *  - the SFR packet path: payload words are queued in the payload FIFO,
 *    writing PKTHDR transmits the packet to the attached DSI peripheral
 *    (kick-dsi.h) and raises SFRPHFIFOEMPTY/SFRPLFIFOEMPTY; read requests
 *    deliver the peripheral response through RXFIFO with RXDATDONE, or a
 *    bus turnaround timeout (TATOUT) when nothing answers;
 *  - INTSRC (write one to clear) / INTMSK (one masks) and the IRQ line;
 *  - SWRST software and function reset; FIFOCTRL pointer init bits.
 *
 * Video mode pixel data are not routed through this model; the LCDIF model
 * scans out the frame buffer itself. Reset values that the vendor driver
 * does not depend on are taken from the Samsung DSIM defaults and are noted
 * as such below.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/kick/imx8mm_mipi_dsim.h"
#include "hw/kick/kick-dsi.h"
#include "migration/vmstate.h"

#define DSIM_VERSION        0x00
#define DSIM_STATUS         0x04
#define DSIM_RGB_STATUS     0x08
#define DSIM_SWRST          0x0c
#define DSIM_CLKCTRL        0x10
#define DSIM_TIMEOUT        0x14
#define DSIM_CONFIG         0x18
#define DSIM_ESCMODE        0x1c
#define DSIM_MDRESOL        0x20
#define DSIM_MVPORCH        0x24
#define DSIM_MHPORCH        0x28
#define DSIM_MSYNC          0x2c
#define DSIM_SDRESOL        0x30
#define DSIM_INTSRC         0x34
#define DSIM_INTMSK         0x38
#define DSIM_PKTHDR         0x3c
#define DSIM_PAYLOAD        0x40
#define DSIM_RXFIFO         0x44
#define DSIM_FIFOTHLD       0x48
#define DSIM_FIFOCTRL       0x4c
#define DSIM_MEMACCHR       0x50
#define DSIM_MULTI_PKT      0x78
#define DSIM_PLLCTRL_1G     0x90
#define DSIM_PLLCTRL        0x94
#define DSIM_PLLCTRL1       0x98
#define DSIM_PLLCTRL2       0x9c
#define DSIM_PLLTMR         0xa0
#define DSIM_PHYTIMING      0xb4
#define DSIM_PHYTIMING1     0xb8
#define DSIM_PHYTIMING2     0xbc
#define DSIM_LAST_REG       DSIM_PHYTIMING2

#define STATUS_PLLSTABLE        BIT(31)
#define STATUS_SWRSTRLS         BIT(20)
#define STATUS_TXREADYHSCLK     BIT(10)
#define STATUS_STOPSTATECLK     BIT(8)

#define SWRST_SWRST             BIT(0)
#define SWRST_FUNCRST           BIT(16)

#define CLKCTRL_TXREQUESTHSCLK  BIT(31)

#define CONFIG_VIDEOMODE        BIT(25)
#define CONFIG_LANEEN_MASK      0x1f

#define ESCMODE_FORCEBTA        BIT(16)
#define ESCMODE_TXTRIGGERRST    BIT(5)

#define MDRESOL_MAINSTANDBY     BIT(31)

#define INTSRC_PLLSTABLE        BIT(31)
#define INTSRC_SWRSTRELEASE     BIT(30)
#define INTSRC_SFRPLFIFOEMPTY   BIT(29)
#define INTSRC_SFRPHFIFOEMPTY   BIT(28)
#define INTSRC_TATOUT           BIT(20)
#define INTSRC_RXDATDONE        BIT(18)
#define INTSRC_RXACK            BIT(16)

#define FIFOCTRL_FULLRX         BIT(25)
#define FIFOCTRL_EMPTYRX        BIT(24)
#define FIFOCTRL_FULLHSFR       BIT(23)
#define FIFOCTRL_EMPTYHSFR      BIT(22)
#define FIFOCTRL_FULLLSFR       BIT(21)
#define FIFOCTRL_EMPTYLSFR      BIT(20)
/* EMPTY flags of the I80, sub and main display FIFOs, which stay empty. */
#define FIFOCTRL_EMPTY_OTHER    (BIT(18) | BIT(16) | BIT(14) | BIT(12) | \
                                 BIT(10) | BIT(8))
#define FIFOCTRL_NINITRX        BIT(4)
#define FIFOCTRL_NINITSFR       BIT(3)
#define FIFOCTRL_NINIT_MASK     0x1f

#define PLLCTRL_PLLEN           BIT(23)

/*
 * Reset values. VERSION comes from the "version" property (the driver
 * checks it against 0x1060200). INTMSK (all sources masked), CLKCTRL
 * (escape prescaler 0xffff), TIMEOUT (BTA and LPDR timeouts at maximum)
 * and FIFOCTRL (all FIFO pointers out of init) follow the Samsung DSIM
 * defaults; they are not verified against the i.MX8MM RM.
 */
#define INTMSK_RESET            0xb337ffff
#define CLKCTRL_RESET           0x0000ffff
#define TIMEOUT_RESET           0x00ffffff

/* MIPI DSI data types the host turns around for a response. */
#define DT_GENERIC_READ_0       0x04
#define DT_GENERIC_READ_1       0x14
#define DT_GENERIC_READ_2       0x24
#define DT_DCS_READ             0x06

static bool dsim_is_long_packet(uint8_t dt)
{
    switch (dt) {
    case 0x09: /* null packet */
    case 0x19: /* blanking packet */
    case 0x29: /* generic long write */
    case 0x39: /* DCS long write */
    case 0x0c: case 0x1c: case 0x2c: /* loosely packed / YCbCr streams */
    case 0x0d: case 0x3d: case 0x3c:
    case 0x0e: case 0x1e: case 0x2e: case 0x3e: /* RGB pixel streams */
        return true;
    default:
        return false;
    }
}

static bool dsim_is_read_request(uint8_t dt)
{
    return dt == DT_GENERIC_READ_0 || dt == DT_GENERIC_READ_1 ||
           dt == DT_GENERIC_READ_2 || dt == DT_DCS_READ;
}

static inline uint32_t dsim_reg(IMX8MMMipiDsimState *s, hwaddr off)
{
    return s->regs[off / 4];
}

static void dsim_update_irq(IMX8MMMipiDsimState *s)
{
    qemu_set_irq(s->irq,
                 !!(dsim_reg(s, DSIM_INTSRC) & ~dsim_reg(s, DSIM_INTMSK)));
}

static void dsim_raise(IMX8MMMipiDsimState *s, uint32_t bits)
{
    s->regs[DSIM_INTSRC / 4] |= bits;
    dsim_update_irq(s);
}

bool imx8mm_dsim_hs_clock_running(IMX8MMMipiDsimState *s)
{
    return !s->in_reset && s->pll_stable &&
           (dsim_reg(s, DSIM_CLKCTRL) & CLKCTRL_TXREQUESTHSCLK);
}

bool imx8mm_dsim_video_active(IMX8MMMipiDsimState *s)
{
    return imx8mm_dsim_hs_clock_running(s) &&
           (dsim_reg(s, DSIM_CONFIG) & CONFIG_VIDEOMODE) &&
           (dsim_reg(s, DSIM_MDRESOL) & MDRESOL_MAINSTANDBY);
}

static uint32_t dsim_status(IMX8MMMipiDsimState *s)
{
    uint32_t status = STATUS_SWRSTRLS;
    uint32_t data_lanes = (dsim_reg(s, DSIM_CONFIG) & CONFIG_LANEEN_MASK) >> 1;

    if (!s->pll_stable) {
        /* PHY not running: no lane reports stop state. */
        return status;
    }
    status |= STATUS_PLLSTABLE;
    if (imx8mm_dsim_hs_clock_running(s)) {
        status |= STATUS_TXREADYHSCLK;
    } else {
        status |= STATUS_STOPSTATECLK;
    }
    /* Data lanes sit in LP-11 stop state unless video is being streamed. */
    if (!imx8mm_dsim_video_active(s)) {
        status |= data_lanes & 0xf;
    }
    return status;
}

static uint32_t dsim_fifoctrl(IMX8MMMipiDsimState *s)
{
    uint32_t v = (dsim_reg(s, DSIM_FIFOCTRL) & FIFOCTRL_NINIT_MASK) |
                 FIFOCTRL_EMPTY_OTHER | FIFOCTRL_EMPTYHSFR;

    if (fifo8_is_empty(&s->rx_fifo)) {
        v |= FIFOCTRL_EMPTYRX;
    }
    if (fifo8_is_full(&s->rx_fifo)) {
        v |= FIFOCTRL_FULLRX;
    }
    if (fifo8_is_empty(&s->pl_fifo)) {
        v |= FIFOCTRL_EMPTYLSFR;
    }
    if (fifo8_num_free(&s->pl_fifo) < 4) {
        v |= FIFOCTRL_FULLLSFR;
    }
    return v;
}

static void dsim_rx_push_word(IMX8MMMipiDsimState *s, uint32_t w)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (fifo8_is_full(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: RX FIFO overflow\n", __func__);
            return;
        }
        fifo8_push(&s->rx_fifo, w >> (8 * i));
    }
}

static void dsim_send_packet(IMX8MMMipiDsimState *s, uint32_t hdr)
{
    uint8_t di = hdr & 0xff;
    uint8_t vc = di >> 6, dt = di & 0x3f;
    uint8_t d0 = (hdr >> 8) & 0xff, d1 = (hdr >> 16) & 0xff;
    g_autofree uint8_t *payload = NULL;
    size_t len;
    KickDsiResponse resp = { 0 };
    bool answered = false;

    if (dsim_is_long_packet(dt)) {
        size_t wc = d0 | (d1 << 8);
        size_t avail = fifo8_num_used(&s->pl_fifo);
        size_t pad = (4 - (wc & 3)) & 3;
        size_t i;

        if (avail < wc) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: long packet WC %zu but only "
                          "%zu payload bytes queued\n", __func__, wc, avail);
            wc = avail;
            pad = 0;
        }
        payload = g_malloc(wc ? wc : 1);
        for (i = 0; i < wc; i++) {
            payload[i] = fifo8_pop(&s->pl_fifo);
        }
        /* The last payload word was written whole; drop its unused bytes. */
        for (i = 0; i < pad && !fifo8_is_empty(&s->pl_fifo); i++) {
            fifo8_pop(&s->pl_fifo);
        }
        len = wc;
    } else {
        payload = g_malloc(2);
        payload[0] = d0;
        payload[1] = d1;
        len = 2;
    }

    if (s->peripheral) {
        KickDsiPeripheralClass *pc = KICK_DSI_PERIPHERAL_GET_CLASS(s->peripheral);

        answered = pc->transfer(KICK_DSI_PERIPHERAL(s->peripheral), vc, dt,
                                payload, len, &resp);
    }

    if (dsim_is_read_request(dt)) {
        if (!answered) {
            /* Nobody drove the bus after the turnaround. */
            dsim_raise(s, INTSRC_TATOUT);
        } else if (dsim_is_long_packet(resp.data_type) ||
                   resp.data_type == 0x1a || resp.data_type == 0x1c) {
            /* Long read response: header with WC, then payload words. */
            size_t i;
            uint16_t n = MIN(resp.len, KICK_DSI_MAX_RESPONSE);

            dsim_rx_push_word(s, (vc << 6) | resp.data_type | (n << 8));
            for (i = 0; i < n; i += 4) {
                uint32_t w = 0;
                size_t j;

                for (j = 0; j < 4 && i + j < n; j++) {
                    w |= resp.data[i + j] << (8 * j);
                }
                dsim_rx_push_word(s, w);
            }
            dsim_raise(s, INTSRC_RXDATDONE);
        } else {
            dsim_rx_push_word(s, (vc << 6) | resp.data_type |
                                 (resp.data[0] << 8) | (resp.data[1] << 16));
            dsim_raise(s, INTSRC_RXDATDONE);
        }
    }

    dsim_raise(s, INTSRC_SFRPHFIFOEMPTY |
                  (fifo8_is_empty(&s->pl_fifo) ? INTSRC_SFRPLFIFOEMPTY : 0));
}

static void dsim_pll_locked(void *opaque)
{
    IMX8MMMipiDsimState *s = opaque;

    if (dsim_reg(s, DSIM_PLLCTRL) & PLLCTRL_PLLEN) {
        s->pll_stable = true;
        dsim_raise(s, INTSRC_PLLSTABLE);
    }
}

static void dsim_pll_enable(IMX8MMMipiDsimState *s)
{
    uint64_t hz = clock_get_hz(s->pll_ref);
    uint64_t cycles = dsim_reg(s, DSIM_PLLTMR);

    if (!hz) {
        hz = s->fallback_pll_ref_hz;
    }
    s->pll_stable = false;
    timer_mod(s->pll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(cycles, NANOSECONDS_PER_SECOND, MAX(hz, 1)));
}

static void dsim_register_reset(IMX8MMMipiDsimState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[DSIM_VERSION / 4] = s->version;
    s->regs[DSIM_CLKCTRL / 4] = CLKCTRL_RESET;
    s->regs[DSIM_TIMEOUT / 4] = TIMEOUT_RESET;
    s->regs[DSIM_INTMSK / 4] = INTMSK_RESET;
    s->regs[DSIM_FIFOCTRL / 4] = FIFOCTRL_NINIT_MASK;
    fifo8_reset(&s->pl_fifo);
    fifo8_reset(&s->rx_fifo);
    s->pll_stable = false;
    timer_del(s->pll_timer);
}

static bool dsim_known_reg(hwaddr off)
{
    return off <= DSIM_MEMACCHR || off == DSIM_MULTI_PKT ||
           (off >= DSIM_PLLCTRL_1G && off <= DSIM_PLLTMR) ||
           (off >= DSIM_PHYTIMING && off <= DSIM_PHYTIMING2);
}

static uint64_t dsim_read(void *opaque, hwaddr off, unsigned size)
{
    IMX8MMMipiDsimState *s = opaque;
    uint32_t v = 0;
    int i;

    if (!dsim_known_reg(off)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }
    if (s->in_reset) {
        return 0;
    }

    switch (off) {
    case DSIM_STATUS:
        return dsim_status(s);
    case DSIM_FIFOCTRL:
        return dsim_fifoctrl(s);
    case DSIM_PKTHDR:
    case DSIM_PAYLOAD:
        return 0;
    case DSIM_RXFIFO:
        if (fifo8_is_empty(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: RX FIFO read while empty\n",
                          __func__);
            return 0;
        }
        for (i = 0; i < 4 && !fifo8_is_empty(&s->rx_fifo); i++) {
            v |= fifo8_pop(&s->rx_fifo) << (8 * i);
        }
        return v;
    default:
        return s->regs[off / 4];
    }
}

static void dsim_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IMX8MMMipiDsimState *s = opaque;
    uint32_t v = value, old;
    int i;

    if (!dsim_known_reg(off)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (s->in_reset) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write while held in reset by the "
                      "display mix\n", __func__);
        return;
    }

    old = s->regs[off / 4];
    switch (off) {
    case DSIM_VERSION:
    case DSIM_STATUS:
    case DSIM_RXFIFO:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, off);
        break;
    case DSIM_SWRST:
        if (v & SWRST_SWRST) {
            dsim_register_reset(s);
            dsim_raise(s, INTSRC_SWRSTRELEASE);
        } else if (v & SWRST_FUNCRST) {
            fifo8_reset(&s->pl_fifo);
            fifo8_reset(&s->rx_fifo);
        }
        /* Both bits are self clearing. */
        break;
    case DSIM_ESCMODE:
        /* Trigger and BTA requests complete at once. */
        if (v & ESCMODE_FORCEBTA) {
            dsim_raise(s, s->peripheral ? INTSRC_RXACK : INTSRC_TATOUT);
        }
        s->regs[off / 4] = v & ~(ESCMODE_FORCEBTA | ESCMODE_TXTRIGGERRST);
        break;
    case DSIM_INTSRC:
        s->regs[off / 4] &= ~v;
        dsim_update_irq(s);
        break;
    case DSIM_INTMSK:
        s->regs[off / 4] = v;
        dsim_update_irq(s);
        break;
    case DSIM_PAYLOAD:
        if (fifo8_num_free(&s->pl_fifo) < 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: payload FIFO overflow\n",
                          __func__);
            break;
        }
        for (i = 0; i < 4; i++) {
            fifo8_push(&s->pl_fifo, v >> (8 * i));
        }
        break;
    case DSIM_PKTHDR:
        s->regs[off / 4] = v;
        dsim_send_packet(s, v);
        break;
    case DSIM_FIFOCTRL:
        /* Clearing a NINIT bit resets that FIFO's pointers. */
        if (!(v & FIFOCTRL_NINITSFR)) {
            fifo8_reset(&s->pl_fifo);
        }
        if (!(v & FIFOCTRL_NINITRX)) {
            fifo8_reset(&s->rx_fifo);
        }
        s->regs[off / 4] = v & FIFOCTRL_NINIT_MASK;
        break;
    case DSIM_PLLCTRL:
        s->regs[off / 4] = v;
        if ((v & PLLCTRL_PLLEN) && !(old & PLLCTRL_PLLEN)) {
            dsim_pll_enable(s);
        } else if (!(v & PLLCTRL_PLLEN)) {
            s->pll_stable = false;
            timer_del(s->pll_timer);
        }
        break;
    default:
        s->regs[off / 4] = v;
        break;
    }
}

static const MemoryRegionOps dsim_ops = {
    .read = dsim_read,
    .write = dsim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* Active-low reset from DISPLAY_BLK_CTRL (MIPI_DSI_I_PRESET / M_RESET). */
static void dsim_reset_n(void *opaque, int n, int level)
{
    IMX8MMMipiDsimState *s = opaque;

    s->in_reset = !level;
    if (s->in_reset) {
        dsim_register_reset(s);
        dsim_update_irq(s);
    }
}

static void dsim_reset_hold(Object *obj, ResetType type)
{
    IMX8MMMipiDsimState *s = IMX8MM_MIPI_DSIM(obj);

    dsim_register_reset(s);
    dsim_update_irq(s);
}

static void dsim_init(Object *obj)
{
    IMX8MMMipiDsimState *s = IMX8MM_MIPI_DSIM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dsim_ops, s, TYPE_IMX8MM_MIPI_DSIM,
                          IMX8MM_DSIM_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->pll_ref = qdev_init_clock_in(DEVICE(obj), "pll-ref", NULL, NULL, 0);
    qdev_init_gpio_in_named(DEVICE(obj), dsim_reset_n, "reset-n", 1);
    fifo8_create(&s->pl_fifo, IMX8MM_DSIM_PL_FIFO_BYTES);
    fifo8_create(&s->rx_fifo, IMX8MM_DSIM_RX_FIFO_BYTES);
}

static void dsim_realize(DeviceState *dev, Error **errp)
{
    IMX8MMMipiDsimState *s = IMX8MM_MIPI_DSIM(dev);

    if (s->peripheral &&
        !object_dynamic_cast(s->peripheral, TYPE_KICK_DSI_PERIPHERAL)) {
        error_setg(errp, "dsi-peripheral does not implement %s",
                   TYPE_KICK_DSI_PERIPHERAL);
        return;
    }
    s->pll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsim_pll_locked, s);
}

static void dsim_finalize(Object *obj)
{
    IMX8MMMipiDsimState *s = IMX8MM_MIPI_DSIM(obj);

    fifo8_destroy(&s->pl_fifo);
    fifo8_destroy(&s->rx_fifo);
}

static const Property dsim_properties[] = {
    /* VERSION register, sec_mipi_dsim-imx.c expects 0x1060200 on the 8MM. */
    DEFINE_PROP_UINT32("version", IMX8MMMipiDsimState, version, 0x01060200),
    /* PLL reference used when no clock is connected to "pll-ref" (DT: 12 MHz). */
    DEFINE_PROP_UINT32("fallback-pll-ref-hz", IMX8MMMipiDsimState,
                       fallback_pll_ref_hz, 12000000),
    DEFINE_PROP_LINK("dsi-peripheral", IMX8MMMipiDsimState, peripheral,
                     TYPE_OBJECT, Object *),
};

static const VMStateDescription vmstate_dsim = {
    .name = TYPE_IMX8MM_MIPI_DSIM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMMipiDsimState, IMX8MM_DSIM_NUM_REGS),
        VMSTATE_FIFO8(pl_fifo, IMX8MMMipiDsimState),
        VMSTATE_FIFO8(rx_fifo, IMX8MMMipiDsimState),
        VMSTATE_BOOL(pll_stable, IMX8MMMipiDsimState),
        VMSTATE_BOOL(in_reset, IMX8MMMipiDsimState),
        VMSTATE_TIMER_PTR(pll_timer, IMX8MMMipiDsimState),
        VMSTATE_CLOCK(pll_ref, IMX8MMMipiDsimState),
        VMSTATE_END_OF_LIST()
    },
};

static void dsim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "i.MX8MM MIPI DSI host (Samsung DSIM)";
    dc->realize = dsim_realize;
    dc->vmsd = &vmstate_dsim;
    device_class_set_props(dc, dsim_properties);
    rc->phases.hold = dsim_reset_hold;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo dsim_types[] = {
    {
        .name = TYPE_IMX8MM_MIPI_DSIM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMMipiDsimState),
        .instance_init = dsim_init,
        .instance_finalize = dsim_finalize,
        .class_init = dsim_class_init,
    },
    {
        .name = TYPE_KICK_DSI_PERIPHERAL,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(KickDsiPeripheralClass),
    },
};

DEFINE_TYPES(dsim_types)
