/*
 * Vivante 2D drawing engine (DE / PE2.0) of the GC520L
 *
 * Executes DRAW_2D commands and VR (filter blit) starts using the 2D states
 * at 0x1200-0x13ff and 0x3400 (state_2d.xml of the etnaviv rnndb).
 *
 * Supported:
 *  - commands CLEAR, LINE, BIT_BLT, BIT_BLT_REVERSED, STRETCH_BLT, and the
 *    filter blits (HOR/VER_FILTER_BLT, ONE_PASS_FILTER_BLT, VR_CONFIG start);
 *  - formats A/X4R4G4B4, A/X1R5G5B5, R5G6B5, A/X8R8G8B8, A8, INDEX8 (source,
 *    through INDEX_COLOR_TABLE32) with the four DE swizzles, and YUY2, UYVY,
 *    NV12, NV16 and YV12 sources with BT.601/BT.709 conversion;
 *  - linear and 4x4 tiled surfaces;
 *  - ROP2/ROP3/ROP4 with source, destination and 8x8 mono or colour pattern;
 *  - source and destination colour keys (PE_TRANSPARENCY);
 *  - PE2.0 alpha blending with the eight blend factors, global alpha modes,
 *    premultiply/demultiply (COLOR_MULTIPLY_MODES);
 *  - clipping, source rotation and destination rotation (ROT_ANGLE, or the
 *    PE1.0 ROTATION bit) and mirroring.
 *
 * Not supported (logged as LOG_UNIMP): monochrome stream sources, super
 * tiling, multi-source blits (the GC520L has no MULTI_SOURCE_BLT feature),
 * dithering, YUV and indexed destinations.
 *
 * Approximations, documented in display-gpu.md: filter blits sample the
 * source bilinearly instead of applying the programmed kernel taps, and the
 * rotation and mirror conventions follow the description in state_2d.xml
 * without having been compared against hardware output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/kick/vivante_gpu.h"

/* 2D state byte addresses. */
#define DE_SRC_ADDRESS              0x1200
#define DE_SRC_STRIDE               0x1204
#define DE_SRC_ROTATION_CONFIG      0x1208
#define DE_SRC_CONFIG               0x120c
#define DE_SRC_ORIGIN               0x1210
#define DE_SRC_SIZE                 0x1214
#define DE_SRC_COLOR_BG             0x1218
#define DE_SRC_COLOR_FG             0x121c
#define DE_STRETCH_FACTOR_LOW       0x1220
#define DE_STRETCH_FACTOR_HIGH      0x1224
#define DE_DEST_ADDRESS             0x1228
#define DE_DEST_STRIDE              0x122c
#define DE_DEST_ROTATION_CONFIG     0x1230
#define DE_DEST_CONFIG              0x1234
#define DE_PATTERN_ADDRESS          0x1238
#define DE_PATTERN_CONFIG           0x123c
#define DE_PATTERN_LOW              0x1240
#define DE_PATTERN_HIGH             0x1244
#define DE_PATTERN_BG_COLOR         0x1250
#define DE_PATTERN_FG_COLOR         0x1254
#define DE_ROP                      0x125c
#define DE_CLIP_TOP_LEFT            0x1260
#define DE_CLIP_BOTTOM_RIGHT        0x1264
#define DE_CONFIG                   0x126c
#define DE_ALPHA_CONTROL            0x127c
#define DE_ALPHA_MODES              0x1280
#define DE_UPLANE_ADDRESS           0x1284
#define DE_UPLANE_STRIDE            0x1288
#define DE_VPLANE_ADDRESS           0x128c
#define DE_VPLANE_STRIDE            0x1290
#define DE_VR_SOURCE_IMAGE_LOW      0x1298
#define DE_VR_SOURCE_IMAGE_HIGH     0x129c
#define DE_VR_SOURCE_ORIGIN_LOW     0x12a0
#define DE_VR_SOURCE_ORIGIN_HIGH    0x12a4
#define DE_VR_TARGET_WINDOW_LOW     0x12a8
#define DE_VR_TARGET_WINDOW_HIGH    0x12ac
#define DE_DEST_ROTATION_HEIGHT     0x12b4
#define DE_SRC_ROTATION_HEIGHT      0x12b8
#define DE_ROT_ANGLE                0x12bc
#define DE_CLEAR_PIXEL_VALUE32      0x12c0
#define DE_DEST_COLOR_KEY           0x12c4
#define DE_GLOBAL_SRC_COLOR         0x12c8
#define DE_GLOBAL_DEST_COLOR        0x12cc
#define DE_COLOR_MULTIPLY_MODES     0x12d0
#define DE_PE_TRANSPARENCY          0x12d4
#define DE_PE_CONTROL               0x12d8
#define DE_SRC_COLOR_KEY_HIGH       0x12dc
#define DE_DEST_COLOR_KEY_HIGH      0x12e0
#define DE_SRC_EX_CONFIG            0x1300
#define DE_INDEX_COLOR_TABLE32      0x3400

/* DE_FORMAT */
enum {
    FMT_X4R4G4B4 = 0, FMT_A4R4G4B4 = 1, FMT_X1R5G5B5 = 2, FMT_A1R5G5B5 = 3,
    FMT_R5G6B5 = 4, FMT_X8R8G8B8 = 5, FMT_A8R8G8B8 = 6, FMT_YUY2 = 7,
    FMT_UYVY = 8, FMT_INDEX8 = 9, FMT_MONOCHROME = 10, FMT_YV12 = 15,
    FMT_A8 = 16, FMT_NV12 = 17, FMT_NV16 = 18,
};

/* DEST_CONFIG.COMMAND */
enum {
    CMD_CLEAR = 0, CMD_LINE = 1, CMD_BIT_BLT = 2, CMD_BIT_BLT_REVERSED = 3,
    CMD_STRETCH_BLT = 4, CMD_HOR_FILTER_BLT = 5, CMD_VER_FILTER_BLT = 6,
    CMD_ONE_PASS_FILTER_BLT = 7, CMD_MULTI_SOURCE_BLT = 8,
};

/* DE_ROT_MODE */
enum { ROT0 = 0, FLIP_X = 1, FLIP_Y = 2, ROT90 = 4, ROT180 = 5, ROT270 = 6 };

/* DE_BLENDMODE */
enum {
    BLEND_ZERO, BLEND_ONE, BLEND_NORMAL, BLEND_INVERSED, BLEND_COLOR,
    BLEND_COLOR_INVERSED, BLEND_SATURATED_ALPHA, BLEND_SATURATED_DEST_ALPHA,
};

/* 2D_TRANSPARENCY_KIND */
enum { TRANSP_OPAQUE = 0, TRANSP_MASK = 1, TRANSP_KEY = 2 };

#define F(v, hi, lo)    (((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define ST(a)           viv_state(s, (a))

/* ------------------------------------------------------------------------ */
/* Page cache shared by every surface access of one operation               */
/* ------------------------------------------------------------------------ */

#define DE_CACHE_PAGES  16
#define DE_PAGE         4096

typedef struct DePage {
    bool valid;
    bool dirty;
    uint32_t va;
    uint64_t lru;
    uint8_t data[DE_PAGE];
} DePage;

typedef struct DeCtx {
    VivGpuState *s;
    DePage pages[DE_CACHE_PAGES];
    uint64_t tick;
} DeCtx;

static void de_flush(DeCtx *c)
{
    int i;

    for (i = 0; i < DE_CACHE_PAGES; i++) {
        DePage *p = &c->pages[i];

        if (p->valid && p->dirty) {
            viv_dma_write(c->s, p->va, p->data, DE_PAGE, VIV_MMU_UNIT_PE);
            p->dirty = false;
        }
    }
}

static DePage *de_page(DeCtx *c, uint32_t va)
{
    uint32_t page = va & ~(DE_PAGE - 1);
    DePage *victim = &c->pages[0];
    int i;

    for (i = 0; i < DE_CACHE_PAGES; i++) {
        DePage *p = &c->pages[i];

        if (p->valid && p->va == page) {
            p->lru = ++c->tick;
            return p;
        }
        if (!p->valid || p->lru < victim->lru) {
            victim = p;
        }
    }
    if (victim->valid && victim->dirty) {
        viv_dma_write(c->s, victim->va, victim->data, DE_PAGE,
                      VIV_MMU_UNIT_PE);
    }
    viv_dma_read(c->s, page, victim->data, DE_PAGE, VIV_MMU_UNIT_PE);
    victim->valid = true;
    victim->dirty = false;
    victim->va = page;
    victim->lru = ++c->tick;
    return victim;
}

static uint32_t de_load(DeCtx *c, uint32_t va, unsigned bytes)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < bytes; i++) {
        DePage *p = de_page(c, va + i);
        v |= p->data[(va + i) & (DE_PAGE - 1)] << (8 * i);
    }
    return v;
}

static void de_store(DeCtx *c, uint32_t va, uint32_t v, unsigned bytes)
{
    unsigned i;

    for (i = 0; i < bytes; i++) {
        DePage *p = de_page(c, va + i);
        p->data[(va + i) & (DE_PAGE - 1)] = v >> (8 * i);
        p->dirty = true;
    }
}

/* ------------------------------------------------------------------------ */
/* Pixel formats                                                            */
/* ------------------------------------------------------------------------ */

typedef struct DeFormat {
    unsigned bpp;           /* bytes per pixel, 0 for planar/special */
    unsigned bits[4];       /* A, R, G, B */
    bool has_alpha;
} DeFormat;

static bool de_format_info(unsigned fmt, DeFormat *f)
{
    static const DeFormat table[] = {
        [FMT_X4R4G4B4] = { 2, { 4, 4, 4, 4 }, false },
        [FMT_A4R4G4B4] = { 2, { 4, 4, 4, 4 }, true },
        [FMT_X1R5G5B5] = { 2, { 1, 5, 5, 5 }, false },
        [FMT_A1R5G5B5] = { 2, { 1, 5, 5, 5 }, true },
        [FMT_R5G6B5]   = { 2, { 0, 5, 6, 5 }, false },
        [FMT_X8R8G8B8] = { 4, { 8, 8, 8, 8 }, false },
        [FMT_A8R8G8B8] = { 4, { 8, 8, 8, 8 }, true },
        [FMT_A8]       = { 1, { 8, 0, 0, 0 }, true },
    };

    if (fmt >= ARRAY_SIZE(table) || !table[fmt].bpp) {
        return false;
    }
    *f = table[fmt];
    return true;
}

/* Bit position of each A, R, G, B component for a DE_SWIZZLE value. */
static void de_shifts(const DeFormat *f, unsigned swizzle, unsigned shift[4])
{
    static const int order[4][4] = {
        { 0, 1, 2, 3 },     /* ARGB */
        { 1, 2, 3, 0 },     /* RGBA */
        { 0, 3, 2, 1 },     /* ABGR */
        { 3, 2, 1, 0 },     /* BGRA */
    };
    unsigned pos = f->bits[0] + f->bits[1] + f->bits[2] + f->bits[3];
    int i;

    for (i = 0; i < 4; i++) {
        int comp = order[swizzle & 3][i];

        pos -= f->bits[comp];
        shift[comp] = pos;
    }
}

static inline uint32_t expand8(uint32_t v, unsigned bits)
{
    return bits ? (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1) : 0;
}

static inline uint32_t reduce8(uint32_t v, unsigned bits)
{
    return bits ? (v * ((1u << bits) - 1) + 127) / 255 : 0;
}

/* Convert a raw pixel to A8R8G8B8. */
static uint32_t de_unpack(const DeFormat *f, unsigned swizzle, uint32_t raw)
{
    unsigned shift[4], i;
    uint32_t c[4];

    de_shifts(f, swizzle, shift);
    for (i = 0; i < 4; i++) {
        c[i] = expand8((raw >> shift[i]) & ((1u << f->bits[i]) - 1),
                       f->bits[i]);
    }
    if (!f->has_alpha) {
        c[0] = 255;
    }
    return (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
}

static uint32_t de_pack(const DeFormat *f, unsigned swizzle, uint32_t argb)
{
    unsigned shift[4], i;
    uint32_t raw = 0;

    de_shifts(f, swizzle, shift);
    for (i = 0; i < 4; i++) {
        uint32_t v = (argb >> (24 - 8 * i)) & 0xff;

        if (i == 0 && !f->has_alpha) {
            /* X bits are written as ones. */
            v = 255;
        }
        raw |= reduce8(v, f->bits[i]) << shift[i];
    }
    return raw;
}

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Limited range YUV to RGB, BT.601 or BT.709, 10-bit fixed point. */
static uint32_t de_yuv_to_argb(int y, int u, int v, bool bt709)
{
    int c = 1192 * (y - 16), d = u - 128, e = v - 128;
    int r, g, b;

    if (bt709) {
        r = c + 1836 * e;
        g = c - 218 * d - 546 * e;
        b = c + 2163 * d;
    } else {
        r = c + 1634 * e;
        g = c - 401 * d - 832 * e;
        b = c + 2066 * d;
    }
    return 0xff000000 | (clamp8(r >> 10) << 16) | (clamp8(g >> 10) << 8) |
           clamp8(b >> 10);
}

/* ------------------------------------------------------------------------ */
/* Surfaces                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct DeSurface {
    uint32_t addr, stride;
    uint32_t uaddr, ustride, vaddr, vstride;
    unsigned format, swizzle, rot;
    uint32_t width, height;     /* physical size, used by rotation */
    bool tiled;
    DeFormat fmt;
    bool valid;
} DeSurface;

static unsigned de_rot_mode(unsigned angle_field, bool pe10_rotation)
{
    if (angle_field) {
        return angle_field;
    }
    return pe10_rotation ? ROT90 : ROT0;
}

static void de_src_surface(VivGpuState *s, DeSurface *sf)
{
    uint32_t cfg = ST(DE_SRC_CONFIG);

    memset(sf, 0, sizeof(*sf));
    sf->addr = ST(DE_SRC_ADDRESS);
    sf->stride = ST(DE_SRC_STRIDE) & 0x3ffff;
    sf->uaddr = ST(DE_UPLANE_ADDRESS);
    sf->ustride = ST(DE_UPLANE_STRIDE) & 0x3ffff;
    sf->vaddr = ST(DE_VPLANE_ADDRESS);
    sf->vstride = ST(DE_VPLANE_STRIDE) & 0x3ffff;
    sf->format = F(cfg, 28, 24);
    sf->swizzle = F(cfg, 21, 20);
    sf->tiled = cfg & BIT(7);
    sf->width = ST(DE_SRC_ROTATION_CONFIG) & 0xffff;
    sf->height = ST(DE_SRC_ROTATION_HEIGHT) & 0xffff;
    sf->rot = de_rot_mode(F(ST(DE_ROT_ANGLE), 2, 0),
                          ST(DE_SRC_ROTATION_CONFIG) & BIT(16));
    sf->valid = true;
    if (cfg & BIT(8)) {
        qemu_log_mask(LOG_UNIMP, "vivante-de: source from the command stream "
                      "(mono expansion / masked blit) not supported\n");
        sf->valid = false;
    }
    if (ST(DE_SRC_EX_CONFIG) & (BIT(0) | BIT(3))) {
        qemu_log_mask(LOG_UNIMP, "vivante-de: multi/super tiled source not "
                      "supported\n");
        sf->valid = false;
    }
    switch (sf->format) {
    case FMT_YUY2: case FMT_UYVY: case FMT_NV12: case FMT_NV16:
    case FMT_YV12: case FMT_INDEX8:
        break;
    default:
        if (!de_format_info(sf->format, &sf->fmt)) {
            qemu_log_mask(LOG_UNIMP, "vivante-de: source format %u not "
                          "supported\n", sf->format);
            sf->valid = false;
        }
        break;
    }
}

static void de_dst_surface(VivGpuState *s, DeSurface *sf)
{
    uint32_t cfg = ST(DE_DEST_CONFIG);

    memset(sf, 0, sizeof(*sf));
    sf->addr = ST(DE_DEST_ADDRESS);
    sf->stride = ST(DE_DEST_STRIDE) & 0x3ffff;
    sf->format = F(cfg, 4, 0);
    sf->swizzle = F(cfg, 17, 16);
    sf->tiled = cfg & BIT(8);
    sf->width = ST(DE_DEST_ROTATION_CONFIG) & 0xffff;
    sf->height = ST(DE_DEST_ROTATION_HEIGHT) & 0xffff;
    sf->rot = de_rot_mode(F(ST(DE_ROT_ANGLE), 5, 3),
                          ST(DE_DEST_ROTATION_CONFIG) & BIT(16));
    sf->valid = de_format_info(sf->format, &sf->fmt);
    if (!sf->valid) {
        qemu_log_mask(LOG_UNIMP, "vivante-de: destination format %u not "
                      "supported\n", sf->format);
    }
}

/*
 * Map logical (rotated) coordinates to the physical surface. ROT90 means
 * the logical picture is the physical one turned 90 degrees clockwise.
 */
static void de_rotate(const DeSurface *sf, int lx, int ly, int *px, int *py)
{
    int w = sf->width, h = sf->height;

    switch (sf->rot) {
    case FLIP_X:
        *px = w - 1 - lx; *py = ly;
        break;
    case FLIP_Y:
        *px = lx; *py = h - 1 - ly;
        break;
    case ROT90:
        *px = ly; *py = h - 1 - lx;
        break;
    case ROT180:
        *px = w - 1 - lx; *py = h - 1 - ly;
        break;
    case ROT270:
        *px = w - 1 - ly; *py = lx;
        break;
    default:
        *px = lx; *py = ly;
        break;
    }
}

/* Byte address of a pixel of a single-plane surface (linear or 4x4 tiled). */
static uint32_t de_addr(const DeSurface *sf, int x, int y, unsigned bpp)
{
    if (sf->tiled) {
        return sf->addr + (y >> 2) * sf->stride +
               (((x >> 2) << 4) + ((y & 3) << 2) + (x & 3)) * bpp;
    }
    return sf->addr + y * sf->stride + x * bpp;
}

static uint32_t de_fetch(DeCtx *c, const DeSurface *sf, int lx, int ly)
{
    VivGpuState *s = c->s;
    bool bt709 = ST(DE_PE_CONTROL) & BIT(0);
    bool vu = ST(DE_PE_CONTROL) & BIT(4);
    int x, y, yy, u, v;
    uint32_t w;

    de_rotate(sf, lx, ly, &x, &y);
    if (x < 0 || y < 0) {
        return 0;
    }

    switch (sf->format) {
    case FMT_YUY2:
    case FMT_UYVY:
        w = de_load(c, sf->addr + y * sf->stride + (x & ~1) * 2, 4);
        if (sf->format == FMT_YUY2) {
            yy = (x & 1) ? F(w, 23, 16) : F(w, 7, 0);
            u = F(w, 15, 8);
            v = F(w, 31, 24);
        } else {
            yy = (x & 1) ? F(w, 31, 24) : F(w, 15, 8);
            u = F(w, 7, 0);
            v = F(w, 23, 16);
        }
        break;
    case FMT_NV12:
    case FMT_NV16: {
        int cy = sf->format == FMT_NV12 ? y >> 1 : y;

        yy = de_load(c, sf->addr + y * sf->stride + x, 1);
        w = de_load(c, sf->uaddr + cy * sf->ustride + (x & ~1), 2);
        u = w & 0xff;
        v = w >> 8;
        break;
    }
    case FMT_YV12:
        yy = de_load(c, sf->addr + y * sf->stride + x, 1);
        u = de_load(c, sf->uaddr + (y >> 1) * sf->ustride + (x >> 1), 1);
        v = de_load(c, sf->vaddr + (y >> 1) * sf->vstride + (x >> 1), 1);
        break;
    case FMT_INDEX8:
        w = de_load(c, de_addr(sf, x, y, 1), 1);
        return ST(DE_INDEX_COLOR_TABLE32 + w * 4);
    default:
        return de_unpack(&sf->fmt, sf->swizzle,
                         de_load(c, de_addr(sf, x, y, sf->fmt.bpp),
                                 sf->fmt.bpp));
    }
    if (vu) {
        int t = u;
        u = v;
        v = t;
    }
    return de_yuv_to_argb(yy, u, v, bt709);
}

static uint32_t de_fetch_dst(DeCtx *c, const DeSurface *sf, int lx, int ly)
{
    int x, y;

    de_rotate(sf, lx, ly, &x, &y);
    return de_unpack(&sf->fmt, sf->swizzle,
                     de_load(c, de_addr(sf, x, y, sf->fmt.bpp), sf->fmt.bpp));
}

static void de_store_dst(DeCtx *c, const DeSurface *sf, int lx, int ly,
                         uint32_t argb)
{
    int x, y;

    de_rotate(sf, lx, ly, &x, &y);
    if (x < 0 || y < 0) {
        return;
    }
    de_store(c, de_addr(sf, x, y, sf->fmt.bpp),
             de_pack(&sf->fmt, sf->swizzle, argb), sf->fmt.bpp);
}

/* ------------------------------------------------------------------------ */
/* Raster operations, pattern, transparency, alpha                          */
/* ------------------------------------------------------------------------ */

typedef struct DeOp {
    unsigned rop_type, rop_fg, rop_bg;
    bool use_src, use_dst, use_pat;
    bool alpha;
    uint32_t alpha_modes, mult_modes, transparency;
    DeSurface pattern;
    uint32_t pat_cfg;
} DeOp;

/* Does the ROP depend on input bit "which" (0 dst, 1 src, 2 pattern)? */
static bool rop_uses(unsigned rop, unsigned type, int which)
{
    unsigned bits = (type == 3 || type == 2) ? 3 : 2;
    unsigned idx, n = 1u << bits;
    int bit;

    if (type == 0) {        /* ROP2_PATTERN: dst bit 0, pattern bit 1 */
        bit = which == 0 ? 0 : which == 2 ? 1 : -1;
    } else if (type == 1) { /* ROP2_SOURCE: dst bit 0, source bit 1 */
        bit = which == 0 ? 0 : which == 1 ? 1 : -1;
    } else {
        bit = which;
    }
    if (bit < 0) {
        return false;
    }
    for (idx = 0; idx < n; idx++) {
        if (((rop >> idx) & 1) != ((rop >> (idx ^ (1u << bit))) & 1)) {
            return true;
        }
    }
    return false;
}

static uint32_t rop_eval(unsigned rop, unsigned type, uint32_t p, uint32_t sr,
                         uint32_t d)
{
    uint32_t in[3], res = 0;
    unsigned nin, idx, i;

    switch (type) {
    case 0:
        in[0] = d; in[1] = p; nin = 2;
        break;
    case 1:
        in[0] = d; in[1] = sr; nin = 2;
        break;
    default:
        in[0] = d; in[1] = sr; in[2] = p; nin = 3;
        break;
    }
    for (idx = 0; idx < (1u << nin); idx++) {
        uint32_t term;

        if (!((rop >> idx) & 1)) {
            continue;
        }
        term = ~0u;
        for (i = 0; i < nin; i++) {
            term &= ((idx >> i) & 1) ? in[i] : ~in[i];
        }
        res |= term;
    }
    return res;
}

static uint32_t de_pattern(DeCtx *c, DeOp *op, int x, int y)
{
    VivGpuState *s = c->s;
    int px = (x + F(op->pat_cfg, 18, 16)) & 7;
    int py = (y + F(op->pat_cfg, 22, 20)) & 7;

    if (op->pat_cfg & BIT(4)) {
        /* Colour pattern: 8x8 pixels at PATTERN_ADDRESS. */
        DeFormat f;

        if (!de_format_info(F(op->pat_cfg, 3, 0), &f)) {
            return 0;
        }
        return de_unpack(&f, 0, de_load(c, ST(DE_PATTERN_ADDRESS) +
                                        (py * 8 + px) * f.bpp, f.bpp));
    }
    /* Mono pattern: one bit per pixel in PATTERN_LOW/HIGH. */
    {
        uint64_t bits = ((uint64_t)ST(DE_PATTERN_HIGH) << 32) |
                        ST(DE_PATTERN_LOW);

        return ((bits >> (py * 8 + px)) & 1) ? ST(DE_PATTERN_FG_COLOR) :
                                               ST(DE_PATTERN_BG_COLOR);
    }
}

static bool key_match(uint32_t c, uint32_t lo, uint32_t hi, bool range)
{
    int i;

    if (!range) {
        return (c & 0xffffff) == (lo & 0xffffff);
    }
    for (i = 0; i < 24; i += 8) {
        uint32_t v = (c >> i) & 0xff;

        if (v < ((lo >> i) & 0xff) || v > ((hi >> i) & 0xff)) {
            return false;
        }
    }
    return true;
}

static uint32_t blend_factor(unsigned mode, uint32_t own_a, uint32_t other_a,
                             uint32_t other_c)
{
    switch (mode) {
    case BLEND_ZERO:
        return 0;
    case BLEND_ONE:
        return 255;
    case BLEND_NORMAL:
        return other_a;
    case BLEND_INVERSED:
        return 255 - other_a;
    case BLEND_COLOR:
        return other_c;
    case BLEND_COLOR_INVERSED:
        return 255 - other_c;
    case BLEND_SATURATED_ALPHA:
        return MIN(own_a, 255 - other_a);
    case BLEND_SATURATED_DEST_ALPHA:
    default:
        return MIN(other_a, 255 - own_a);
    }
}

static inline uint32_t mul8(uint32_t a, uint32_t b)
{
    uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

/*
 * PE2.0 blending: Porter-Duff style, the source factor is chosen from the
 * destination alpha/colour and the destination factor from the source.
 */
static uint32_t de_blend(VivGpuState *s, DeOp *op, uint32_t src, uint32_t dst)
{
    uint32_t m = op->alpha_modes, mm = op->mult_modes;
    uint32_t gs = ST(DE_GLOBAL_SRC_COLOR), gd = ST(DE_GLOBAL_DEST_COLOR);
    uint32_t sa = src >> 24, da = dst >> 24;
    uint32_t sc[3], dc[3], out = 0, oa;
    unsigned fs_mode = F(m, 26, 24), fd_mode = F(m, 30, 28);
    int i;

    for (i = 0; i < 3; i++) {
        sc[i] = (src >> (16 - 8 * i)) & 0xff;
        dc[i] = (dst >> (16 - 8 * i)) & 0xff;
    }

    /* Global alpha: replace or scale. */
    switch (F(m, 9, 8)) {
    case 1: sa = gs >> 24; break;
    case 2: sa = mul8(sa, gs >> 24); break;
    }
    switch (F(m, 13, 12)) {
    case 1: da = gd >> 24; break;
    case 2: da = mul8(da, gd >> 24); break;
    }
    if (m & BIT(0)) {
        sa = 255 - sa;
    }
    if (m & BIT(4)) {
        da = 255 - da;
    }

    /* COLOR_MULTIPLY_MODES */
    for (i = 0; i < 3; i++) {
        if (mm & BIT(0)) {
            sc[i] = mul8(sc[i], src >> 24);
        }
        switch (F(mm, 9, 8)) {
        case 1: sc[i] = mul8(sc[i], gs >> 24); break;
        case 2: sc[i] = mul8(sc[i], (gs >> (16 - 8 * i)) & 0xff); break;
        }
        if (mm & BIT(4)) {
            dc[i] = mul8(dc[i], dst >> 24);
        }
    }

    for (i = 0; i < 3; i++) {
        uint32_t fs = blend_factor(fs_mode, sa, da, dc[i]);
        uint32_t fd = blend_factor(fd_mode, da, sa, sc[i]);
        uint32_t v = MIN(mul8(sc[i], fs) + mul8(dc[i], fd), 255u);

        out |= v << (16 - 8 * i);
    }
    oa = MIN(mul8(sa, blend_factor(fs_mode, sa, da, da)) +
             mul8(da, blend_factor(fd_mode, da, sa, sa)), 255u);

    if ((mm & BIT(20)) && oa) {
        /* DST_DEMULTIPLY: divide the result colour by its alpha. */
        uint32_t r = 0;

        for (i = 0; i < 3; i++) {
            uint32_t v = (out >> (16 - 8 * i)) & 0xff;
            r |= MIN(v * 255 / oa, 255u) << (16 - 8 * i);
        }
        out = r;
    }
    return (oa << 24) | out;
}

static void de_op_init(VivGpuState *s, DeOp *op)
{
    uint32_t rop = ST(DE_ROP);

    memset(op, 0, sizeof(*op));
    op->rop_type = F(rop, 21, 20);
    op->rop_fg = F(rop, 7, 0);
    op->rop_bg = F(rop, 15, 8);
    op->alpha = ST(DE_ALPHA_CONTROL) & BIT(0);
    op->alpha_modes = ST(DE_ALPHA_MODES);
    op->mult_modes = ST(DE_COLOR_MULTIPLY_MODES);
    op->transparency = ST(DE_PE_TRANSPARENCY);
    op->pat_cfg = ST(DE_PATTERN_CONFIG);
    if (op->alpha) {
        op->use_src = op->use_dst = true;
    } else {
        int which;

        for (which = 0; which < 3; which++) {
            bool u = rop_uses(op->rop_fg, op->rop_type, which) ||
                     (op->rop_type == 3 &&
                      rop_uses(op->rop_bg, op->rop_type, which));
            if (which == 0) {
                op->use_dst = u;
            } else if (which == 1) {
                op->use_src = u;
            } else {
                op->use_pat = u;
            }
        }
    }
}

/*
 * Combine one pixel. Returns false if the destination is left untouched
 * (transparent pixel without ROP4).
 */
static bool de_pixel(DeCtx *c, DeOp *op, uint32_t src, uint32_t dst,
                     int x, int y, uint32_t *out)
{
    VivGpuState *s = c->s;
    uint32_t t = op->transparency;
    bool transparent = false;
    uint32_t pat = 0;

    if (F(t, 1, 0) == TRANSP_KEY &&
        key_match(src, ST(DE_SRC_COLOR_BG), ST(DE_SRC_COLOR_KEY_HIGH),
                  t & BIT(29))) {
        transparent = true;
    }
    if (F(t, 9, 8) == TRANSP_KEY &&
        !key_match(dst, ST(DE_DEST_COLOR_KEY), ST(DE_DEST_COLOR_KEY_HIGH),
                   t & BIT(29))) {
        /* Destination key: only pixels matching the key are written. */
        transparent = true;
    }

    if (op->alpha) {
        if (transparent) {
            return false;
        }
        *out = de_blend(s, op, src, dst);
        return true;
    }
    if (transparent && op->rop_type != 3) {
        return false;
    }
    if (op->use_pat) {
        pat = de_pattern(c, op, x, y);
    }
    *out = rop_eval(transparent ? op->rop_bg : op->rop_fg, op->rop_type, pat,
                    src, dst);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Operations                                                               */
/* ------------------------------------------------------------------------ */

typedef struct DeRect {
    int x0, y0, x1, y1;     /* bottom/right exclusive */
} DeRect;

static bool de_clip(VivGpuState *s, const DeRect *r, DeRect *out)
{
    uint32_t tl = ST(DE_CLIP_TOP_LEFT), br = ST(DE_CLIP_BOTTOM_RIGHT);

    out->x0 = MAX(r->x0, (int)F(tl, 14, 0));
    out->y0 = MAX(r->y0, (int)F(tl, 30, 16));
    out->x1 = MIN(r->x1, (int)F(br, 14, 0));
    out->y1 = MIN(r->y1, (int)F(br, 30, 16));
    return out->x0 < out->x1 && out->y0 < out->y1;
}

/* Bilinear sample at a 16.16 source position (filter blit approximation). */
static uint32_t de_sample_bilinear(DeCtx *c, const DeSurface *sf,
                                   int64_t fx, int64_t fy, const DeRect *lim)
{
    int x0 = fx >> 16, y0 = fy >> 16;
    uint32_t wx = (fx >> 8) & 0xff, wy = (fy >> 8) & 0xff;
    int x1 = MIN(x0 + 1, lim->x1 - 1), y1 = MIN(y0 + 1, lim->y1 - 1);
    uint32_t p[4], out = 0;
    int i;

    x0 = MAX(MIN(x0, lim->x1 - 1), lim->x0);
    y0 = MAX(MIN(y0, lim->y1 - 1), lim->y0);
    x1 = MAX(x1, lim->x0);
    y1 = MAX(y1, lim->y0);
    p[0] = de_fetch(c, sf, x0, y0);
    p[1] = de_fetch(c, sf, x1, y0);
    p[2] = de_fetch(c, sf, x0, y1);
    p[3] = de_fetch(c, sf, x1, y1);
    for (i = 0; i < 32; i += 8) {
        uint32_t a = (p[0] >> i) & 0xff, b = (p[1] >> i) & 0xff;
        uint32_t cc = (p[2] >> i) & 0xff, d = (p[3] >> i) & 0xff;
        uint32_t top = a * (256 - wx) + b * wx;
        uint32_t bot = cc * (256 - wx) + d * wx;

        out |= ((top * (256 - wy) + bot * wy) >> 16) << i;
    }
    return out;
}

enum { SRC_NONE, SRC_DIRECT, SRC_STRETCH, SRC_FILTER };

typedef struct DeBlit {
    DeRect rect;            /* destination rectangle as programmed */
    DeRect clip;            /* clipped area to write */
    int mode;
    int src_x, src_y;       /* source origin for DIRECT/STRETCH */
    bool relative;
    uint32_t fx, fy;        /* 16.16 stretch factors */
    int64_t ox, oy;         /* 16.16 filter origin */
    DeRect src_lim;         /* source image bounds for filtering */
    bool mirror_src_x, mirror_src_y, mirror_dst_x, mirror_dst_y;
    bool reverse;
    uint32_t solid;         /* CLEAR value */
    bool clear;
} DeBlit;

static void de_execute(DeCtx *c, DeSurface *src, DeSurface *dst, DeOp *op,
                       DeBlit *b)
{
    int w = b->rect.x1 - b->rect.x0, h = b->rect.y1 - b->rect.y0;
    int row, col;
    int ystart = b->reverse ? b->clip.y1 - 1 : b->clip.y0;
    int ystep = b->reverse ? -1 : 1;
    int ncols = b->clip.x1 - b->clip.x0;
    g_autofree uint32_t *srow = g_new(uint32_t, ncols);

    for (row = 0; row < b->clip.y1 - b->clip.y0; row++) {
        int dy = ystart + row * ystep;
        int offy = dy - b->rect.y0;

        if (b->mirror_src_y) {
            offy = h - 1 - offy;
        }
        /* Fetch the whole source row first so overlapping copies work. */
        for (col = 0; col < ncols; col++) {
            int dx = b->clip.x0 + col;
            int offx = dx - b->rect.x0;

            if (b->mirror_src_x) {
                offx = w - 1 - offx;
            }
            switch (b->mode) {
            case SRC_DIRECT:
                srow[col] = b->relative ?
                    de_fetch(c, src, dx + b->src_x, dy + b->src_y) :
                    de_fetch(c, src, b->src_x + offx, b->src_y + offy);
                break;
            case SRC_STRETCH:
                srow[col] = de_fetch(c, src,
                    b->src_x + (int)(((uint64_t)offx * b->fx) >> 16),
                    b->src_y + (int)(((uint64_t)offy * b->fy) >> 16));
                break;
            case SRC_FILTER:
                srow[col] = de_sample_bilinear(c, src,
                    b->ox + (int64_t)offx * b->fx,
                    b->oy + (int64_t)offy * b->fy, &b->src_lim);
                break;
            default:
                srow[col] = 0;
                break;
            }
        }
        for (col = 0; col < ncols; col++) {
            int dx = b->clip.x0 + col;
            int wx = b->mirror_dst_x ? b->rect.x0 + b->rect.x1 - 1 - dx : dx;
            int wy = b->mirror_dst_y ? b->rect.y0 + b->rect.y1 - 1 - dy : dy;
            uint32_t d = 0, out;

            if (b->clear) {
                de_store_dst(c, dst, wx, wy, b->solid);
                continue;
            }
            if (op->use_dst || F(op->transparency, 9, 8) == TRANSP_KEY) {
                d = de_fetch_dst(c, dst, wx, wy);
            }
            if (de_pixel(c, op, srow[col], d, wx, wy, &out)) {
                de_store_dst(c, dst, wx, wy, out);
            }
        }
    }
}

static void de_mirror_flags(VivGpuState *s, DeBlit *b)
{
    uint32_t angle = ST(DE_ROT_ANGLE), cfg = ST(DE_CONFIG);
    unsigned sm = F(angle, 13, 12), dm = F(angle, 17, 16);

    if (cfg & BIT(0)) {
        /* Legacy MIRROR_BLT applies to the destination. */
        dm |= F(cfg, 5, 4);
    }
    b->mirror_src_x = sm & 1;
    b->mirror_src_y = sm & 2;
    b->mirror_dst_x = dm & 1;
    b->mirror_dst_y = dm & 2;
}

/* Bresenham line from (x0,y0) towards (x1,y1), end point excluded. */
static void de_line(DeCtx *c, DeSurface *dst, DeOp *op, const DeRect *r)
{
    VivGpuState *s = c->s;
    int x = r->x0, y = r->y0;
    int dx = abs(r->x1 - r->x0), sx = r->x0 < r->x1 ? 1 : -1;
    int dy = -abs(r->y1 - r->y0), sy = r->y0 < r->y1 ? 1 : -1;
    int err = dx + dy;
    DeRect pt, clipped;

    while (x != r->x1 || y != r->y1) {
        uint32_t d = 0, out;

        pt = (DeRect){ x, y, x + 1, y + 1 };
        if (de_clip(s, &pt, &clipped)) {
            if (op->use_dst) {
                d = de_fetch_dst(c, dst, x, y);
            }
            if (de_pixel(c, op, ST(DE_SRC_COLOR_FG), d, x, y, &out)) {
                de_store_dst(c, dst, x, y, out);
            }
        }
        if (2 * err >= dy) {
            err += dy;
            x += sx;
        }
        if (2 * err <= dx) {
            err += dx;
            y += sy;
        }
    }
}

void viv_de_draw(VivGpuState *s, const uint32_t *rects, unsigned nrects,
                 const uint32_t *data, unsigned ndata)
{
    unsigned cmd = F(ST(DE_DEST_CONFIG), 15, 12);
    g_autofree DeCtx *c = g_new0(DeCtx, 1);
    DeSurface src, dst;
    DeOp op;
    unsigned i;

    c->s = s;
    de_dst_surface(s, &dst);
    if (!dst.valid) {
        return;
    }
    de_op_init(s, &op);
    if (ndata) {
        qemu_log_mask(LOG_UNIMP, "vivante-de: %u inline data words of a "
                      "DRAW_2D ignored (stream sources not supported)\n",
                      ndata);
    }

    switch (cmd) {
    case CMD_CLEAR:
    case CMD_LINE:
    case CMD_BIT_BLT:
    case CMD_BIT_BLT_REVERSED:
    case CMD_STRETCH_BLT:
        break;
    case CMD_HOR_FILTER_BLT:
    case CMD_VER_FILTER_BLT:
    case CMD_ONE_PASS_FILTER_BLT:
        /* DRAW_2D rectangles with a filter command use the stretch path. */
        break;
    case CMD_MULTI_SOURCE_BLT:
    default:
        qemu_log_mask(LOG_UNIMP, "vivante-de: DEST_CONFIG command %u not "
                      "supported\n", cmd);
        return;
    }

    if (cmd != CMD_CLEAR && cmd != CMD_LINE && op.use_src) {
        de_src_surface(s, &src);
        if (!src.valid) {
            return;
        }
    } else {
        memset(&src, 0, sizeof(src));
    }

    for (i = 0; i < nrects; i++) {
        DeRect r = {
            .x0 = (int16_t)F(rects[2 * i], 15, 0),
            .y0 = (int16_t)F(rects[2 * i], 31, 16),
            .x1 = (int16_t)F(rects[2 * i + 1], 15, 0),
            .y1 = (int16_t)F(rects[2 * i + 1], 31, 16),
        };
        DeBlit b = { .rect = r };
        uint32_t origin = ST(DE_SRC_ORIGIN);

        if (cmd == CMD_LINE) {
            de_line(c, &dst, &op, &r);
            continue;
        }
        if (!de_clip(s, &r, &b.clip)) {
            continue;
        }
        de_mirror_flags(s, &b);
        b.reverse = cmd == CMD_BIT_BLT_REVERSED;
        b.relative = ST(DE_SRC_CONFIG) & BIT(6);
        b.src_x = b.relative ? (int16_t)F(origin, 15, 0) : F(origin, 15, 0);
        b.src_y = b.relative ? (int16_t)F(origin, 31, 16) : F(origin, 31, 16);
        b.fx = ST(DE_STRETCH_FACTOR_LOW) & 0x7fffffff;
        b.fy = ST(DE_STRETCH_FACTOR_HIGH) & 0x7fffffff;

        if (cmd == CMD_CLEAR) {
            b.clear = true;
            b.solid = ST(DE_CLEAR_PIXEL_VALUE32);
        } else if (!op.use_src) {
            b.mode = SRC_NONE;
        } else if (cmd == CMD_BIT_BLT || cmd == CMD_BIT_BLT_REVERSED) {
            b.mode = SRC_DIRECT;
        } else {
            b.mode = SRC_STRETCH;
        }
        de_execute(c, &src, &dst, &op, &b);
    }
    de_flush(c);
}

/*
 * VR_CONFIG.START: filter blit of VR_SOURCE_IMAGE into VR_TARGET_WINDOW,
 * stepping through the source from VR_SOURCE_ORIGIN by STRETCH_FACTOR
 * (all 16.16). The kernel taps in FILTER_KERNEL are not applied; the
 * source is sampled bilinearly.
 */
void viv_de_vr_start(VivGpuState *s, uint32_t mode)
{
    g_autofree DeCtx *c = g_new0(DeCtx, 1);
    DeSurface src, dst;
    DeOp op;
    DeBlit b = { 0 };
    uint32_t lo, hi;

    qemu_log_mask(LOG_UNIMP, "vivante-de: filter blit (VR mode %u) uses "
                  "bilinear sampling, kernel coefficients ignored\n", mode);
    c->s = s;
    de_dst_surface(s, &dst);
    de_src_surface(s, &src);
    if (!dst.valid || !src.valid) {
        return;
    }
    de_op_init(s, &op);
    op.use_src = true;

    lo = ST(DE_VR_TARGET_WINDOW_LOW);
    hi = ST(DE_VR_TARGET_WINDOW_HIGH);
    b.rect = (DeRect){ F(lo, 15, 0), F(lo, 31, 16), F(hi, 15, 0),
                       F(hi, 31, 16) };
    lo = ST(DE_VR_SOURCE_IMAGE_LOW);
    hi = ST(DE_VR_SOURCE_IMAGE_HIGH);
    b.src_lim = (DeRect){ F(lo, 15, 0), F(lo, 31, 16), F(hi, 15, 0),
                          F(hi, 31, 16) };
    if (b.src_lim.x0 >= b.src_lim.x1 || b.src_lim.y0 >= b.src_lim.y1 ||
        !de_clip(s, &b.rect, &b.clip)) {
        return;
    }
    b.mode = SRC_FILTER;
    b.ox = (int32_t)ST(DE_VR_SOURCE_ORIGIN_LOW);
    b.oy = (int32_t)ST(DE_VR_SOURCE_ORIGIN_HIGH);
    b.fx = ST(DE_STRETCH_FACTOR_LOW) & 0x7fffffff;
    b.fy = ST(DE_STRETCH_FACTOR_HIGH) & 0x7fffffff;
    /* A horizontal or vertical pass only scales along its own axis. */
    if (mode == 0) {
        b.fy = 1 << 16;
    } else if (mode == 1) {
        b.fx = 1 << 16;
    }
    de_mirror_flags(s, &b);
    de_execute(c, &src, &dst, &op, &b);
    de_flush(c);
}
