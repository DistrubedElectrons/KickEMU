/*
 * i.MX8M Mini eLCDIF display controller
 *
 * Register model of the LCDIF at 0x32e00000 (GIC SPI 5) as programmed by
 * the vendor "imx-lcdif" driver (drivers/gpu/imx/lcdif/lcdif-common.c and
 * drivers/gpu/drm/imx/lcdif/ in varigit linux-imx 5.4). The register layout
 * follows the eLCDIF of the i.MX6/7 family that the i.MX8MM reuses, with the
 * 8MM specific PIGEON_12_x crop registers at 0xb00.
 *
 * Only the DOTCLK (RGB) interface is modelled because that is the only mode
 * used on the 8MM, where the LCDIF output always feeds the MIPI DSIM. The
 * frame timing is derived from VDCTRL0..4 and the pixel clock, and drives
 * the VSYNC_EDGE and CUR_FRAME_DONE interrupts. The frame buffer at CUR_BUF
 * is scanned out into a QEMU graphic console with dirty tracking.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "hw/kick/imx8mm_lcdif.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* Register offsets. CTRL, CTRL1, CTRL2 and VDCTRL0 have SET/CLR/TOG aliases. */
#define LCDIF_CTRL              0x000
#define LCDIF_CTRL1             0x010
#define LCDIF_CTRL2             0x020
#define LCDIF_TRANSFER_COUNT    0x030
#define LCDIF_CUR_BUF           0x040
#define LCDIF_NEXT_BUF          0x050
#define LCDIF_TIMING            0x060
#define LCDIF_VDCTRL0           0x070
#define LCDIF_VDCTRL1           0x080
#define LCDIF_VDCTRL2           0x090
#define LCDIF_VDCTRL3           0x0a0
#define LCDIF_VDCTRL4           0x0b0
#define LCDIF_DVICTRL0          0x0c0
#define LCDIF_DVICTRL4          0x100
#define LCDIF_CSC_COEFF0        0x110
#define LCDIF_CSC_LIMIT         0x170
#define LCDIF_DATA              0x180
#define LCDIF_BM_ERROR_STAT     0x190
#define LCDIF_CRC_STAT          0x1a0
#define LCDIF_STAT              0x1b0
#define LCDIF_THRES             0x200
#define LCDIF_AS_CTRL           0x210
#define LCDIF_SYNC_DELAY        0x260
#define LCDIF_PIGEONCTRL0       0x380
#define LCDIF_PIGEONCTRL2       0x3a0
#define LCDIF_PIGEON_BASE       0x800
#define LCDIF_PIGEON_12_0       0xb00
#define LCDIF_PIGEON_12_1       0xb10

#define REG_SET                 0x4
#define REG_CLR                 0x8
#define REG_TOG                 0xc

#define R(off)                  ((off) >> 4)

/* CTRL */
#define CTRL_SFTRST             BIT(31)
#define CTRL_CLKGATE            BIT(30)
#define CTRL_SHIFT_DIR          BIT(26)
#define CTRL_SHIFT_NUM_SHIFT    21
#define CTRL_SHIFT_NUM_MASK     (0x1f << CTRL_SHIFT_NUM_SHIFT)
#define CTRL_DOTCLK_MODE        BIT(17)
#define CTRL_INPUT_SWIZZLE_SHIFT 14
#define CTRL_WORD_LENGTH_SHIFT  8
#define CTRL_MASTER             BIT(5)
#define CTRL_DF16               BIT(3)
#define CTRL_RUN                BIT(0)
#define CTRL_RESET              (CTRL_SFTRST | CTRL_CLKGATE)

/* CTRL1 */
#define CTRL1_FIFO_CLEAR        BIT(21)
#define CTRL1_BYTE_PACKING_SHIFT 16
#define CTRL1_IRQ_SHIFT         8
#define CTRL1_IRQ_EN_SHIFT      12
#define CTRL1_VSYNC_EDGE_IRQ    BIT(8)
#define CTRL1_CUR_FRAME_DONE_IRQ BIT(9)
#define CTRL1_UNDERFLOW_IRQ     BIT(10)
#define CTRL1_OVERFLOW_IRQ      BIT(11)
#define CTRL1_IRQ_MASK          (0xf << CTRL1_IRQ_SHIFT)
#define CTRL1_RESET             (0xf << CTRL1_BYTE_PACKING_SHIFT)

/* CTRL2 */
#define CTRL2_ODD_LINE_SHIFT    16
#define CTRL2_RESET             0x00200000  /* OUTSTANDING_REQS = REQ_2 */

/* TRANSFER_COUNT */
#define TRANSFER_COUNT_RESET    0x00010000

/* VDCTRL0 */
#define VDCTRL0_VSYNC_PERIOD_UNIT BIT(21)

/* VDCTRL3 */
#define VDCTRL3_VERT_WAIT_MASK  0xffff

/* STAT: PRESENT, LFIFO_EMPTY, TXFIFO_EMPTY, DVI_CURRENT_FIELD */
#define STAT_PRESENT            BIT(31)
#define STAT_LFIFO_EMPTY        BIT(28)
#define STAT_TXFIFO_EMPTY       BIT(26)
#define STAT_BUSY               BIT(25)
#define STAT_DVI_CURRENT_FIELD  BIT(24)

/* AS_CTRL */
#define AS_CTRL_AS_ENABLE       BIT(0)

/* PIGEON_12_0 / PIGEON_12_1: horizontal crop used by lcdif_set_fb_hcrop() */
#define PIGEON_12_0_EN          BIT(0)
#define PIGEON_12_1_CLR_CNT(v)  (((v) >> 16) & 0xffff)

/* Source pixel layouts derived from CTRL/CTRL1. */
enum {
    LCDIF_FMT_NONE,
    LCDIF_FMT_RGB565,
    LCDIF_FMT_ARGB1555,
    LCDIF_FMT_RGB666_UNPACKED,
    LCDIF_FMT_RGB888_PACKED,
    LCDIF_FMT_XRGB8888,
};

static inline uint32_t lcdif_reg(IMX8MMLCDIFState *s, hwaddr off)
{
    return s->regs[R(off)];
}

static void lcdif_update_irq(IMX8MMLCDIFState *s)
{
    uint32_t ctrl1 = lcdif_reg(s, LCDIF_CTRL1);
    uint32_t pending = (ctrl1 >> CTRL1_IRQ_SHIFT) & 0xf;
    uint32_t enabled = (ctrl1 >> CTRL1_IRQ_EN_SHIFT) & 0xf;

    qemu_set_irq(s->irq, !!(pending & enabled));
}

static void lcdif_raise(IMX8MMLCDIFState *s, uint32_t bits)
{
    s->regs[R(LCDIF_CTRL1)] |= bits;
    lcdif_update_irq(s);
}

static bool lcdif_running(IMX8MMLCDIFState *s)
{
    uint32_t ctrl = lcdif_reg(s, LCDIF_CTRL);

    return (ctrl & CTRL_RUN) && !(ctrl & CTRL_SFTRST);
}

/*
 * Frame period in nanoseconds. VDCTRL2 holds the line length in pixel
 * clocks, VDCTRL1 the frame length in lines (VSYNC_PERIOD_UNIT set, which is
 * what the driver programs) or in pixel clocks.
 */
static int64_t lcdif_frame_period_ns(IMX8MMLCDIFState *s, uint64_t *lines)
{
    uint64_t htotal = lcdif_reg(s, LCDIF_VDCTRL2) & 0x3ffff;
    uint64_t vtotal = lcdif_reg(s, LCDIF_VDCTRL1);
    uint64_t pixels;
    uint64_t hz = clock_get_hz(s->pix_clk);

    if (lcdif_reg(s, LCDIF_VDCTRL0) & VDCTRL0_VSYNC_PERIOD_UNIT) {
        pixels = htotal * vtotal;
        *lines = vtotal;
    } else {
        pixels = vtotal;
        *lines = htotal ? vtotal / htotal : 0;
    }

    if (hz && pixels) {
        return muldiv64(pixels, NANOSECONDS_PER_SECOND, hz);
    }
    /* No pixel clock known: fall back to the configured refresh rate. */
    return NANOSECONDS_PER_SECOND / MAX(s->fallback_refresh_hz, 1);
}

/*
 * Two timer events per frame: the VSYNC edge at the start of the frame,
 * where NEXT_BUF is latched into CUR_BUF, and CUR_FRAME_DONE after the last
 * active line has been fetched.
 */
static void lcdif_frame_event(void *opaque)
{
    IMX8MMLCDIFState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t lines;
    int64_t period = lcdif_frame_period_ns(s, &lines);

    if (!s->frame_active) {
        if (!lcdif_running(s)) {
            return;
        }
        s->frame_active = true;
        s->frame_start_ns = now;
        if (s->regs[R(LCDIF_CUR_BUF)] != s->regs[R(LCDIF_NEXT_BUF)]) {
            s->regs[R(LCDIF_CUR_BUF)] = s->regs[R(LCDIF_NEXT_BUF)];
        }
        lcdif_raise(s, CTRL1_VSYNC_EDGE_IRQ);

        /* Frame done once VERT_WAIT_CNT + VCOUNT lines have gone by. */
        uint64_t done_lines = (lcdif_reg(s, LCDIF_VDCTRL3) & VDCTRL3_VERT_WAIT_MASK) +
                              (lcdif_reg(s, LCDIF_TRANSFER_COUNT) >> 16);
        int64_t done = period;
        if (lines && done_lines < lines) {
            done = muldiv64(period, done_lines, lines);
        }
        timer_mod(s->frame_timer, now + done);
    } else {
        s->frame_active = false;
        lcdif_raise(s, CTRL1_CUR_FRAME_DONE_IRQ);
        if (lcdif_running(s)) {
            timer_mod(s->frame_timer, s->frame_start_ns + period);
        }
    }
}

static void lcdif_start(IMX8MMLCDIFState *s)
{
    uint32_t ctrl = lcdif_reg(s, LCDIF_CTRL);

    if (!(ctrl & CTRL_DOTCLK_MODE)) {
        qemu_log_mask(LOG_UNIMP, "%s: only the DOTCLK interface is modelled "
                      "(CTRL=0x%08x)\n", __func__, ctrl);
        return;
    }
    if (!(ctrl & CTRL_MASTER)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: RUN without MASTER, no frames "
                      "will be fetched\n", __func__);
        return;
    }
    if (!s->frame_active && !timer_pending(s->frame_timer)) {
        s->invalidate = true;
        timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void lcdif_soft_reset(IMX8MMLCDIFState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R(LCDIF_CTRL)] = CTRL_RESET;
    s->regs[R(LCDIF_CTRL1)] = CTRL1_RESET;
    s->regs[R(LCDIF_CTRL2)] = CTRL2_RESET;
    s->regs[R(LCDIF_TRANSFER_COUNT)] = TRANSFER_COUNT_RESET;
    s->frame_active = false;
    timer_del(s->frame_timer);
    s->invalidate = true;
    lcdif_update_irq(s);
}

static bool lcdif_has_sct(hwaddr base)
{
    return base == LCDIF_CTRL || base == LCDIF_CTRL1 || base == LCDIF_CTRL2 ||
           base == LCDIF_VDCTRL0;
}

static bool lcdif_known_reg(hwaddr base)
{
    if (base <= LCDIF_VDCTRL4 || base == LCDIF_STAT ||
        base == LCDIF_BM_ERROR_STAT || base == LCDIF_CRC_STAT ||
        base == LCDIF_THRES || base == LCDIF_DATA) {
        return true;
    }
    if (base >= LCDIF_DVICTRL0 && base <= LCDIF_DVICTRL4) {
        return true;
    }
    if (base >= LCDIF_CSC_COEFF0 && base <= LCDIF_CSC_LIMIT) {
        return true;
    }
    if (base >= LCDIF_AS_CTRL && base <= LCDIF_SYNC_DELAY) {
        return true;
    }
    if (base >= LCDIF_PIGEONCTRL0 && base <= LCDIF_PIGEONCTRL2) {
        return true;
    }
    /* PIGEON_n_0..2 at 0x800 + n * 0x40, n = 0..15 */
    if (base >= LCDIF_PIGEON_BASE && base < IMX8MM_LCDIF_REG_SPACE) {
        return ((base - LCDIF_PIGEON_BASE) & 0x30) != 0x30;
    }
    return false;
}

static uint64_t lcdif_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMLCDIFState *s = opaque;
    hwaddr base = offset & ~0xf;

    if (base >= IMX8MM_LCDIF_REG_SPACE || !lcdif_known_reg(base) ||
        ((offset & 0xf) && !lcdif_has_sct(base))) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    switch (base) {
    case LCDIF_STAT: {
        uint32_t stat = STAT_PRESENT | STAT_LFIFO_EMPTY | STAT_TXFIFO_EMPTY |
                        STAT_DVI_CURRENT_FIELD;
        if (lcdif_running(s)) {
            stat |= STAT_BUSY;
        }
        return stat;
    }
    case LCDIF_DATA:
        /* Write-only data port of the MPU interface. */
        return 0;
    default:
        return s->regs[R(base)];
    }
}

static void lcdif_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    IMX8MMLCDIFState *s = opaque;
    hwaddr base = offset & ~0xf;
    uint32_t old, val;

    if (base >= IMX8MM_LCDIF_REG_SPACE || !lcdif_known_reg(base) ||
        ((offset & 0xf) && !lcdif_has_sct(base))) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    old = s->regs[R(base)];
    switch (offset & 0xf) {
    case REG_SET:
        val = old | value;
        break;
    case REG_CLR:
        val = old & ~value;
        break;
    case REG_TOG:
        val = old ^ value;
        break;
    default:
        val = value;
        break;
    }

    /* While SFTRST is set the block is held in reset. */
    if (base != LCDIF_CTRL && (lcdif_reg(s, LCDIF_CTRL) & CTRL_SFTRST)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to 0x%" HWADDR_PRIx
                      " while in soft reset ignored\n", __func__, offset);
        return;
    }

    switch (base) {
    case LCDIF_CTRL:
        if ((val & CTRL_SFTRST) && !(old & CTRL_SFTRST)) {
            lcdif_soft_reset(s);
            return;
        }
        s->regs[R(base)] = val;
        if ((val & CTRL_RUN) && !(old & CTRL_RUN)) {
            lcdif_start(s);
        }
        s->invalidate = true;
        break;
    case LCDIF_CTRL1:
        /* FIFO_CLEAR is self-contained: nothing is buffered in the model. */
        s->regs[R(base)] = val;
        lcdif_update_irq(s);
        s->invalidate = true;
        break;
    case LCDIF_STAT:
    case LCDIF_CRC_STAT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, offset);
        break;
    case LCDIF_DATA:
        qemu_log_mask(LOG_UNIMP, "%s: MPU data port is not modelled\n",
                      __func__);
        break;
    case LCDIF_AS_CTRL:
        if (val & AS_CTRL_AS_ENABLE) {
            qemu_log_mask(LOG_UNIMP, "%s: alpha surface overlay is not "
                          "modelled, AS_CTRL=0x%08x\n", __func__, val);
        }
        s->regs[R(base)] = val;
        break;
    case LCDIF_CUR_BUF:
    case LCDIF_NEXT_BUF:
    default:
        s->regs[R(base)] = val;
        s->invalidate = true;
        break;
    }
}

static const MemoryRegionOps lcdif_ops = {
    .read = lcdif_read,
    .write = lcdif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------------ */
/* Scanout                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Decode the input pixel layout. The vendor driver programs:
 *   RGB565/BGR565:   WORD_LENGTH 0 (16 bit), DF16 = 0, BYTE_PACKING 0xf
 *   xRGB1555 family: WORD_LENGTH 0, DF16 = 1, BYTE_PACKING 0xf
 *   xRGB8888 family: WORD_LENGTH 3 (24 bit), BYTE_PACKING 0x7, with
 *                    SHIFT_DIR/SHIFT_NUM = right/8 for RGBA/RGBX
 * CTRL2 ODD/EVEN_LINE_PATTERN selects the output component order.
 */
static int lcdif_decode_format(IMX8MMLCDIFState *s, unsigned *bpp)
{
    uint32_t ctrl = lcdif_reg(s, LCDIF_CTRL);
    uint32_t packing = (lcdif_reg(s, LCDIF_CTRL1) >> CTRL1_BYTE_PACKING_SHIFT) & 0xf;
    unsigned word_length = (ctrl >> CTRL_WORD_LENGTH_SHIFT) & 3;

    switch (word_length) {
    case 0: /* 16 bit */
        *bpp = 2;
        return (ctrl & CTRL_DF16) ? LCDIF_FMT_ARGB1555 : LCDIF_FMT_RGB565;
    case 2: /* 18 bit, one pixel per 32-bit word */
        *bpp = 4;
        return LCDIF_FMT_RGB666_UNPACKED;
    case 3: /* 24 bit */
        if (packing == 0x7) {
            *bpp = 4;
            return LCDIF_FMT_XRGB8888;
        }
        if (packing == 0xf) {
            *bpp = 3;
            return LCDIF_FMT_RGB888_PACKED;
        }
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "%s: unsupported input format WORD_LENGTH=%u "
                  "BYTE_PACKING=0x%x\n", __func__, word_length, packing);
    *bpp = 0;
    return LCDIF_FMT_NONE;
}

static inline uint32_t lcdif_swizzle(uint32_t w, unsigned swizzle)
{
    switch (swizzle) {
    case 1: /* swap all bytes */
        return bswap32(w);
    case 2: /* swap half-words */
        return (w << 16) | (w >> 16);
    case 3: /* swap bytes within each half-word */
        return ((w & 0x00ff00ff) << 8) | ((w >> 8) & 0x00ff00ff);
    default:
        return w;
    }
}

/* Expand an n-bit component to 8 bits. */
static inline uint32_t expand(uint32_t v, unsigned bits)
{
    return (v << (8 - bits)) | (v >> (2 * bits - 8));
}

static void lcdif_draw_line(void *opaque, uint8_t *d, const uint8_t *src,
                            int width, int deststep)
{
    IMX8MMLCDIFState *s = opaque;
    uint32_t ctrl = lcdif_reg(s, LCDIF_CTRL);
    unsigned swizzle = (ctrl >> CTRL_INPUT_SWIZZLE_SHIFT) & 3;
    unsigned shift = (ctrl & CTRL_SHIFT_NUM_MASK) >> CTRL_SHIFT_NUM_SHIFT;
    bool shift_right = ctrl & CTRL_SHIFT_DIR;
    unsigned pattern = (lcdif_reg(s, LCDIF_CTRL2) >> CTRL2_ODD_LINE_SHIFT) & 7;
    uint32_t *dst = (uint32_t *)d;
    int x;

    for (x = 0; x < width; x++) {
        uint32_t w, c2, c1, c0, r, g, b;

        switch (s->src_format) {
        case LCDIF_FMT_RGB565:
            w = lduw_le_p(src);
            src += 2;
            c2 = expand((w >> 11) & 0x1f, 5);
            c1 = expand((w >> 5) & 0x3f, 6);
            c0 = expand(w & 0x1f, 5);
            break;
        case LCDIF_FMT_ARGB1555:
            w = lduw_le_p(src);
            src += 2;
            c2 = expand((w >> 10) & 0x1f, 5);
            c1 = expand((w >> 5) & 0x1f, 5);
            c0 = expand(w & 0x1f, 5);
            break;
        case LCDIF_FMT_RGB666_UNPACKED:
            w = lcdif_swizzle(ldl_le_p(src), swizzle);
            src += 4;
            c2 = expand((w >> 12) & 0x3f, 6);
            c1 = expand((w >> 6) & 0x3f, 6);
            c0 = expand(w & 0x3f, 6);
            break;
        case LCDIF_FMT_RGB888_PACKED:
            c0 = src[0];
            c1 = src[1];
            c2 = src[2];
            src += 3;
            break;
        case LCDIF_FMT_XRGB8888:
        default:
            w = lcdif_swizzle(ldl_le_p(src), swizzle);
            src += 4;
            w = shift_right ? w >> shift : w << shift;
            c2 = (w >> 16) & 0xff;
            c1 = (w >> 8) & 0xff;
            c0 = w & 0xff;
            break;
        }

        /* CTRL2 line pattern: order in which components leave the bus. */
        switch (pattern) {
        case 1: /* RBG */
            r = c2; b = c1; g = c0;
            break;
        case 2: /* GBR */
            g = c2; b = c1; r = c0;
            break;
        case 3: /* GRB */
            g = c2; r = c1; b = c0;
            break;
        case 4: /* BRG */
            b = c2; r = c1; g = c0;
            break;
        case 5: /* BGR */
            b = c2; g = c1; r = c0;
            break;
        default: /* RGB */
            r = c2; g = c1; b = c0;
            break;
        }
        *dst = (r << 16) | (g << 8) | b;
        dst = (uint32_t *)((uint8_t *)dst + deststep);
    }
}

static void lcdif_invalidate(void *opaque)
{
    IMX8MMLCDIFState *s = opaque;

    s->invalidate = true;
}

static void lcdif_blank(IMX8MMLCDIFState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (surface) {
        memset(surface_data(surface), 0,
               surface_stride(surface) * surface_height(surface));
        dpy_gfx_update_full(s->con);
    }
}

static void lcdif_update_display(void *opaque)
{
    IMX8MMLCDIFState *s = opaque;
    DisplaySurface *surface;
    uint32_t count = lcdif_reg(s, LCDIF_TRANSFER_COUNT);
    uint32_t width = count & 0xffff;
    uint32_t height = count >> 16;
    uint32_t out_width = width;
    hwaddr base = lcdif_reg(s, LCDIF_CUR_BUF);
    unsigned bpp;
    int format, first = 0, last = 0;

    if (!lcdif_running(s)) {
        if (s->invalidate) {
            lcdif_blank(s);
            s->invalidate = false;
        }
        return;
    }

    format = lcdif_decode_format(s, &bpp);
    if (format == LCDIF_FMT_NONE || !width || !height) {
        return;
    }

    /* PIGEON_12 masks DE after CLR_CNT pixels: the visible width. */
    if ((lcdif_reg(s, LCDIF_PIGEON_12_0) & PIGEON_12_0_EN) &&
        PIGEON_12_1_CLR_CNT(lcdif_reg(s, LCDIF_PIGEON_12_1))) {
        out_width = MIN(width,
                        PIGEON_12_1_CLR_CNT(lcdif_reg(s, LCDIF_PIGEON_12_1)));
    }

    if (format != s->src_format || width != s->fb_width ||
        height != s->fb_height || out_width != s->out_width ||
        base != s->fb_base) {
        s->invalidate = true;
    }

    surface = qemu_console_surface(s->con);
    if (!surface || surface_width(surface) != out_width ||
        surface_height(surface) != height) {
        qemu_console_resize(s->con, out_width, height);
        surface = qemu_console_surface(s->con);
        s->invalidate = true;
    }

    if (s->invalidate) {
        s->src_format = format;
        s->fb_width = width;
        s->fb_height = height;
        s->out_width = out_width;
        s->fb_base = base;
        s->src_stride = width * bpp;
        framebuffer_update_memory_section(&s->fbsection, s->dma_mr, base,
                                          height, s->src_stride);
    }

    if (!s->fbsection.mr) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: frame buffer at 0x%" HWADDR_PRIx
                      " is not in RAM\n", __func__, base);
        return;
    }

    framebuffer_update_display(surface, &s->fbsection, out_width, height,
                               s->src_stride, surface_stride(surface),
                               sizeof(uint32_t), s->invalidate,
                               lcdif_draw_line, s, &first, &last);
    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, out_width, last - first + 1);
    }
    s->invalidate = false;
}

static const GraphicHwOps lcdif_gfx_ops = {
    .invalidate = lcdif_invalidate,
    .gfx_update = lcdif_update_display,
};

/* ------------------------------------------------------------------------ */

static void lcdif_reset_hold(Object *obj, ResetType type)
{
    IMX8MMLCDIFState *s = IMX8MM_LCDIF(obj);

    lcdif_soft_reset(s);
}

static void lcdif_init(Object *obj)
{
    IMX8MMLCDIFState *s = IMX8MM_LCDIF(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &lcdif_ops, s, TYPE_IMX8MM_LCDIF,
                          IMX8MM_LCDIF_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->pix_clk = qdev_init_clock_in(DEVICE(obj), "pix", NULL, NULL,
                                    ClockUpdate);
}

static void lcdif_realize(DeviceState *dev, Error **errp)
{
    IMX8MMLCDIFState *s = IMX8MM_LCDIF(dev);

    if (!s->panel_width || !s->panel_height) {
        error_setg(errp, "panel-width and panel-height must be non-zero");
        return;
    }
    if (!s->dma_mr) {
        s->dma_mr = get_system_memory();
    }
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcdif_frame_event, s);
    s->con = graphic_console_init(dev, 0, &lcdif_gfx_ops, s);
    qemu_console_resize(s->con, s->panel_width, s->panel_height);
}

static const Property lcdif_properties[] = {
    /* Console size before the guest programs a mode. */
    DEFINE_PROP_UINT32("panel-width", IMX8MMLCDIFState, panel_width, 800),
    DEFINE_PROP_UINT32("panel-height", IMX8MMLCDIFState, panel_height, 480),
    /* Frame rate used when no pixel clock is connected to the "pix" input. */
    DEFINE_PROP_UINT32("fallback-refresh-hz", IMX8MMLCDIFState,
                       fallback_refresh_hz, 60),
    DEFINE_PROP_LINK("dma-mr", IMX8MMLCDIFState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static int lcdif_post_load(void *opaque, int version_id)
{
    IMX8MMLCDIFState *s = opaque;

    s->invalidate = true;
    return 0;
}

static const VMStateDescription vmstate_lcdif = {
    .name = TYPE_IMX8MM_LCDIF,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = lcdif_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX8MMLCDIFState, IMX8MM_LCDIF_NUM_REGS),
        VMSTATE_BOOL(frame_active, IMX8MMLCDIFState),
        VMSTATE_INT64(frame_start_ns, IMX8MMLCDIFState),
        VMSTATE_TIMER_PTR(frame_timer, IMX8MMLCDIFState),
        VMSTATE_CLOCK(pix_clk, IMX8MMLCDIFState),
        VMSTATE_END_OF_LIST()
    },
};

static void lcdif_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "i.MX8MM eLCDIF display controller";
    dc->realize = lcdif_realize;
    dc->vmsd = &vmstate_lcdif;
    device_class_set_props(dc, lcdif_properties);
    rc->phases.hold = lcdif_reset_hold;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo lcdif_info = {
    .name = TYPE_IMX8MM_LCDIF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX8MMLCDIFState),
    .instance_init = lcdif_init,
    .class_init = lcdif_class_init,
};

static void lcdif_register_types(void)
{
    type_register_static(&lcdif_info);
}

type_init(lcdif_register_types)
