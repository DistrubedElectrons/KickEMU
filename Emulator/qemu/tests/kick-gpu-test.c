/*
 * qtest for the Vivante GPU cores of the i.MX8MM (GC600 3D, GC520L 2D)
 *
 * Checks the identification registers, the galcore reset sequence, the
 * wait/link front end with events, the MMUv2 table walk and exceptions,
 * and 2D clears and blits verified in guest memory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define GPU3D               0x38000000
#define GPU2D               0x38008000
#define RAM                 0x40000000

#define CLOCK_CONTROL       0x000
#define IDLE_STATE          0x004
#define INTR_ACK            0x010
#define INTR_ENBL           0x014
#define CHIP_FEATURE        0x01c
#define CHIP_MODEL          0x020
#define CHIP_REV            0x024
#define CUSTOMER_ID         0x030
#define MINOR_FEATURE_0     0x034
#define MINOR_FEATURE_1     0x074
#define PRODUCT_ID          0x0a8
#define ECO_ID              0x0e8
#define PM_MODULE_CONTROLS  0x104
#define PM_PULSE_EATER      0x10c
#define MMU_SAFE            0x180
#define MMU_CONFIG          0x184
#define MMU_STATUS          0x188
#define MMU_CONTROL         0x18c
#define MMU_EXC_ADDR        0x190
#define FE_CMD_ADDR         0x654
#define FE_CMD_CTRL         0x658
#define FE_DMA_ADDR         0x664
#define FE_DMA_LOW          0x668

#define OP(op)              ((uint32_t)(op) << 27)
#define LOAD_STATE(addr, n) (OP(1) | ((n) << 16) | ((addr) >> 2))
#define END                 OP(2)
#define DRAW_2D(n)          (OP(4) | ((n) << 8))
#define WAIT(cyc)           (OP(7) | (cyc))
#define LINK(pf)            (OP(8) | (pf))
#define STALL               OP(9)

#define GL_PIPE_SELECT      0x3800
#define GL_EVENT            0x3804
#define GL_SEMAPHORE        0x3808
#define EVENT_FROM_PE       (1u << 6)
#define EVENT_FROM_FE       (1u << 5)

#define STEP                (1000 * 1000)

typedef struct CmdBuf {
    QTestState *qts;
    uint64_t base;
    uint32_t pos;           /* bytes */
} CmdBuf;

static void emit(CmdBuf *b, uint32_t w)
{
    qtest_writel(b->qts, b->base + b->pos, w);
    b->pos += 4;
}

static void emit_state(CmdBuf *b, uint32_t addr, uint32_t v)
{
    emit(b, LOAD_STATE(addr, 1));
    emit(b, v);
}

static void emit_event(CmdBuf *b, unsigned id, uint32_t from)
{
    emit_state(b, GL_EVENT, id | from);
}

static void emit_end(CmdBuf *b)
{
    emit(b, END);
    emit(b, 0);
}

static QTestState *start(void)
{
    return qtest_init("-machine kick-display-bench -display none");
}

static uint32_t rd(QTestState *qts, uint64_t core, uint32_t off)
{
    return qtest_readl(qts, core + off);
}

static void wr(QTestState *qts, uint64_t core, uint32_t off, uint32_t v)
{
    qtest_writel(qts, core + off, v);
}

static void fe_start(QTestState *qts, uint64_t core, uint32_t addr,
                     uint32_t bytes)
{
    wr(qts, core, INTR_ENBL, ~0u);
    wr(qts, core, FE_CMD_ADDR, addr);
    wr(qts, core, FE_CMD_CTRL, (1u << 16) | ((bytes + 7) >> 3));
}

static void test_ids(void)
{
    QTestState *qts = start();

    /* GC600 rev 0x4653 product 0x70005 (galcore gc7000nanoultra rc3a). */
    g_assert_cmphex(rd(qts, GPU3D, CHIP_MODEL), ==, 0x600);
    g_assert_cmphex(rd(qts, GPU3D, CHIP_REV), ==, 0x4653);
    g_assert_cmphex(rd(qts, GPU3D, PRODUCT_ID), ==, 0x70005);
    g_assert_cmphex(rd(qts, GPU3D, ECO_ID), ==, 0);
    g_assert_cmphex(rd(qts, GPU3D, CUSTOMER_ID), ==, 0);
    g_assert_cmphex(rd(qts, GPU3D, CHIP_FEATURE) & 0x204, ==, 0x004);
    /* MORE_MINOR_FEATURES, then MMU_VERSION (MMUv2). */
    g_assert_cmphex(rd(qts, GPU3D, MINOR_FEATURE_0) & (1u << 21), !=, 0);
    g_assert_cmphex(rd(qts, GPU3D, MINOR_FEATURE_1) & (1u << 28), !=, 0);

    /* GC520 rev 0x5341 product 0x5202 (galcore gc520l rc1b). */
    g_assert_cmphex(rd(qts, GPU2D, CHIP_MODEL), ==, 0x520);
    g_assert_cmphex(rd(qts, GPU2D, CHIP_REV), ==, 0x5341);
    g_assert_cmphex(rd(qts, GPU2D, PRODUCT_ID), ==, 0x5202);
    g_assert_cmphex(rd(qts, GPU2D, CHIP_FEATURE) & 0x204, ==, 0x200);
    g_assert_cmphex(rd(qts, GPU2D, MINOR_FEATURE_1) & (1u << 28), !=, 0);

    g_assert_cmphex(rd(qts, GPU3D, IDLE_STATE), ==, 0x7fffffff);
    qtest_quit(qts);
}

/* galcore _ResetGPU() */
static void test_reset_sequence(void)
{
    QTestState *qts = start();
    uint32_t control = 0x00010900 | (1u << 19);

    wr(qts, GPU3D, PM_MODULE_CONTROLS, 0);
    wr(qts, GPU3D, PM_PULSE_EATER, 0x015b0880 | (1u << 17));
    wr(qts, GPU3D, CLOCK_CONTROL, 0x00010900 | (1u << 9));
    wr(qts, GPU3D, CLOCK_CONTROL, 0x00010900);
    wr(qts, GPU3D, CLOCK_CONTROL, control);
    wr(qts, GPU3D, CLOCK_CONTROL, control | (1u << 12));
    wr(qts, GPU3D, CLOCK_CONTROL, control);
    wr(qts, GPU3D, CLOCK_CONTROL, control & ~(1u << 19));
    g_assert_cmphex(rd(qts, GPU3D, IDLE_STATE) & 1, ==, 1);
    g_assert_cmphex(rd(qts, GPU3D, CLOCK_CONTROL) & (3u << 16), ==, 3u << 16);
    g_assert_cmphex(rd(qts, GPU3D, MMU_CONTROL) & 1, ==, 0);
    g_assert_cmphex(rd(qts, GPU3D, PM_PULSE_EATER), ==, 0);
    qtest_quit(qts);
}

static void test_fe_events(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, RAM + 0x10000, 0 };
    CmdBuf n = { qts, RAM + 0x20000, 0 };
    uint32_t wait_addr, end_addr, v;

    qtest_irq_intercept_out_named(qts, "/machine/gpu3d", "sysbus-irq");

    /* Kernel command queue: event 5, then the WAIT/LINK idle loop. */
    emit(&b, STALL);
    emit(&b, 0x0701);
    emit_event(&b, 5, EVENT_FROM_FE);
    wait_addr = b.base + b.pos;
    emit(&b, WAIT(200));
    emit(&b, 0);
    emit(&b, LINK(2));
    emit(&b, wait_addr);

    fe_start(qts, GPU3D, b.base, b.pos);
    qtest_clock_step(qts, STEP);
    g_assert_true(qtest_get_irq(qts, 0));

    /* gckHARDWARE_QueryIdle(): FE busy in the loop, other modules idle. */
    g_assert_cmphex(rd(qts, GPU3D, IDLE_STATE) | (1u << 14), ==, 0x7ffffffe);
    v = rd(qts, GPU3D, FE_DMA_ADDR);
    g_assert_cmphex(v, >=, wait_addr);
    g_assert_cmphex(v, <, wait_addr + 16);
    v = rd(qts, GPU3D, FE_DMA_LOW) >> 27;
    g_assert_true(v == 7 || v == 8);

    /* Read to clear. */
    g_assert_cmphex(rd(qts, GPU3D, INTR_ACK), ==, 1u << 5);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, GPU3D, INTR_ACK), ==, 0);

    /* Commit: new buffer with two events and END, linked from the WAIT. */
    emit_event(&n, 7, EVENT_FROM_PE);
    emit(&n, LOAD_STATE(GL_EVENT, 1) | 0);
    emit(&n, 9 | EVENT_FROM_FE);
    end_addr = n.base + n.pos;
    emit_end(&n);
    qtest_writel(qts, wait_addr + 4, n.base);
    qtest_writel(qts, wait_addr, LINK((n.pos + 7) >> 3));

    qtest_clock_step(qts, 10 * STEP);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, GPU3D, INTR_ACK), ==, (1u << 7) | (1u << 9));
    /* gckHARDWARE_GetIdle(): idle and DMA address past the END. */
    g_assert_cmphex(rd(qts, GPU3D, IDLE_STATE), ==, 0x7fffffff);
    g_assert_cmphex(rd(qts, GPU3D, FE_DMA_ADDR), ==, end_addr + 8);
    qtest_quit(qts);
}

/*
 * MMUv2 in 1K mode: MTLB of 256 entries covering 16 MiB each, STLB of
 * 4096 4 KiB pages. Maps GPU VA 0x10000000.. to RAM + 0x300000.
 */
#define MTLB                (RAM + 0x100000)
#define STLB                (RAM + 0x104000)
#define SAFE                (RAM + 0x10c000)
#define MAPPED_PA           (RAM + 0x300000)
#define MAPPED_VA           0x10000000

static void mmu_setup(QTestState *qts, uint64_t core, unsigned pages)
{
    CmdBuf b = { qts, RAM + 0x110000, 0 };
    unsigned i;

    for (i = 0; i < 256; i++) {
        qtest_writel(qts, MTLB + i * 4, 0);
    }
    qtest_writel(qts, MTLB + (MAPPED_VA >> 24) * 4, (uint32_t)STLB | 1);
    for (i = 0; i < 4096; i++) {
        uint32_t e = i < pages ? (uint32_t)(MAPPED_PA + i * 4096) | 5 : 0;
        qtest_writel(qts, STLB + i * 4, e);
    }

    /* _ProgramMMUStates() for gcvSECURE_NONE, then END. */
    emit_state(&b, 0x0184, (uint32_t)MTLB | 1);
    emit_state(&b, 0x0180, (uint32_t)SAFE);
    emit_state(&b, 0x3848, 1u << 16);
    emit_end(&b);
    fe_start(qts, core, b.base, b.pos);
    qtest_clock_step(qts, STEP);
    g_assert_cmphex(rd(qts, core, IDLE_STATE), ==, 0x7fffffff);
    g_assert_cmphex(rd(qts, core, MMU_CONFIG), ==, (uint32_t)MTLB | 1);
    g_assert_cmphex(rd(qts, core, MMU_SAFE), ==, (uint32_t)SAFE);
    /* gckHARDWARE_SetMMU(): enable through the AHB register. */
    wr(qts, core, MMU_CONTROL, 1);
}

static void test_mmu(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, MAPPED_PA, 0 };

    qtest_irq_intercept_out_named(qts, "/machine/gpu3d", "sysbus-irq");
    mmu_setup(qts, GPU3D, 16);

    /* A command buffer fetched through the MMU. */
    emit_event(&b, 3, EVENT_FROM_FE);
    emit_end(&b);
    fe_start(qts, GPU3D, MAPPED_VA, b.pos);
    qtest_clock_step(qts, STEP);
    g_assert_cmphex(rd(qts, GPU3D, INTR_ACK), ==, 1u << 3);
    g_assert_cmphex(rd(qts, GPU3D, FE_DMA_ADDR), ==, MAPPED_VA + b.pos);

    /* LINK into an unmapped 16 MiB region: MTLB entry not present. */
    b.pos = 0;
    emit(&b, LINK(2));
    emit(&b, 0x20000000);
    fe_start(qts, GPU3D, MAPPED_VA, b.pos);
    qtest_clock_step(qts, STEP);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(rd(qts, GPU3D, INTR_ACK) & (1u << 30), ==, 1u << 30);
    g_assert_cmphex(rd(qts, GPU3D, MMU_STATUS) & 0xf, ==, 1);
    g_assert_cmphex(rd(qts, GPU3D, MMU_EXC_ADDR), ==, 0x20000000);
    g_assert_cmphex(rd(qts, GPU3D, IDLE_STATE), ==, 0x7fffffff);

    /* Inside the mapped region but beyond the 16 mapped pages. */
    b.pos = 0;
    emit(&b, LINK(2));
    emit(&b, MAPPED_VA + 16 * 4096);
    fe_start(qts, GPU3D, MAPPED_VA, b.pos);
    qtest_clock_step(qts, STEP);
    g_assert_cmphex(rd(qts, GPU3D, INTR_ACK) & (1u << 30), ==, 1u << 30);
    g_assert_cmphex(rd(qts, GPU3D, MMU_STATUS) & 0xf, ==, 2);
    g_assert_cmphex(rd(qts, GPU3D, MMU_EXC_ADDR), ==, MAPPED_VA + 16 * 4096);
    qtest_quit(qts);
}

static void test_3d_draw_completes(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, RAM + 0x10000, 0 };

    /* A 3D draw is skipped but the following event still fires. */
    emit_state(&b, GL_PIPE_SELECT, 0);
    emit(&b, OP(5));
    emit(&b, 4);
    emit(&b, 0);
    emit(&b, 3);
    emit_state(&b, GL_SEMAPHORE, 0x0701);
    emit(&b, STALL);
    emit(&b, 0x0701);
    emit_event(&b, 1, EVENT_FROM_PE);
    emit_end(&b);
    fe_start(qts, GPU3D, b.base, b.pos);
    qtest_clock_step(qts, STEP);
    g_assert_cmphex(rd(qts, GPU3D, INTR_ACK), ==, 1u << 1);
    g_assert_cmphex(rd(qts, GPU3D, IDLE_STATE), ==, 0x7fffffff);
    qtest_quit(qts);
}

/* ---------------------------------------------------------------------- */
/* 2D                                                                      */
/* ---------------------------------------------------------------------- */

#define DE_SRC_ADDRESS          0x1200
#define DE_SRC_STRIDE           0x1204
#define DE_SRC_ROTATION_CONFIG  0x1208
#define DE_SRC_CONFIG           0x120c
#define DE_SRC_ORIGIN           0x1210
#define DE_STRETCH_FACTOR_LOW   0x1220
#define DE_STRETCH_FACTOR_HIGH  0x1224
#define DE_DEST_ADDRESS         0x1228
#define DE_DEST_STRIDE          0x122c
#define DE_DEST_ROTATION_CONFIG 0x1230
#define DE_DEST_CONFIG          0x1234
#define DE_ROP                  0x125c
#define DE_CLIP_TOP_LEFT        0x1260
#define DE_CLIP_BOTTOM_RIGHT    0x1264
#define DE_ALPHA_CONTROL        0x127c
#define DE_ALPHA_MODES          0x1280
#define DE_ROT_ANGLE            0x12bc
#define DE_CLEAR_PIXEL_VALUE32  0x12c0

#define FMT_R5G6B5              4
#define FMT_A8R8G8B8            6
#define CMD_CLEAR               0
#define CMD_BIT_BLT             2
#define CMD_STRETCH_BLT         4
#define ROP3                    (2u << 20)

#define DST_PA                  (RAM + 0x400000)
#define SRC_PA                  (RAM + 0x500000)
#define SW                      32
#define SH                      16

static void emit_rect(CmdBuf *b, int x0, int y0, int x1, int y1)
{
    emit(b, DRAW_2D(1));
    emit(b, 0);
    emit(b, (y0 << 16) | x0);
    emit(b, (y1 << 16) | x1);
}

static void emit_dest(CmdBuf *b, uint32_t addr, unsigned cmd)
{
    emit_state(b, GL_PIPE_SELECT, 1);
    emit_state(b, DE_DEST_ADDRESS, addr);
    emit_state(b, DE_DEST_STRIDE, SW * 4);
    emit_state(b, DE_DEST_ROTATION_CONFIG, SW);
    emit_state(b, DE_DEST_CONFIG, FMT_A8R8G8B8 | (cmd << 12));
    emit_state(b, DE_CLIP_TOP_LEFT, 0);
    emit_state(b, DE_CLIP_BOTTOM_RIGHT, (SH << 16) | SW);
}

static void run_2d(QTestState *qts, CmdBuf *b)
{
    emit_event(b, 0, EVENT_FROM_PE);
    emit_end(b);
    fe_start(qts, GPU2D, b->base, b->pos);
    qtest_clock_step(qts, STEP);
    g_assert_cmphex(rd(qts, GPU2D, INTR_ACK), ==, 1);
}

static uint32_t px(QTestState *qts, uint64_t base, int x, int y)
{
    return qtest_readl(qts, base + (y * SW + x) * 4);
}

static void clear_mem(QTestState *qts, uint64_t base, uint32_t v)
{
    int i;

    for (i = 0; i < SW * SH; i++) {
        qtest_writel(qts, base + i * 4, v);
    }
}

static void test_2d_clear(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, RAM + 0x10000, 0 };
    int x, y;

    clear_mem(qts, DST_PA, 0x11111111);
    emit_dest(&b, DST_PA, CMD_CLEAR);
    emit_state(&b, DE_ROP, ROP3 | 0xcc);
    emit_state(&b, DE_CLEAR_PIXEL_VALUE32, 0xff336699);
    /* Clipped on the right: only x < 20 is written. */
    emit_state(&b, DE_CLIP_BOTTOM_RIGHT, (SH << 16) | 20);
    emit_rect(&b, 4, 2, 24, 10);
    run_2d(qts, &b);

    for (y = 0; y < SH; y++) {
        for (x = 0; x < SW; x++) {
            bool in = x >= 4 && x < 20 && y >= 2 && y < 10;
            g_assert_cmphex(px(qts, DST_PA, x, y), ==,
                            in ? 0xff336699 : 0x11111111);
        }
    }
    qtest_quit(qts);
}

static void fill_src565(QTestState *qts)
{
    int x, y;

    for (y = 0; y < SH; y++) {
        for (x = 0; x < SW; x++) {
            /* r = x, g = y, b = x ^ y, 5/6/5 bits */
            uint16_t p = ((x & 0x1f) << 11) | ((y & 0x3f) << 5) |
                         ((x ^ y) & 0x1f);
            qtest_writew(qts, SRC_PA + (y * SW + x) * 2, p);
        }
    }
}

static uint32_t argb_from565(int x, int y)
{
    uint32_t r = x & 0x1f, g = y & 0x3f, b = (x ^ y) & 0x1f;

    r = (r * 255 + 15) / 31;
    g = (g * 255 + 31) / 63;
    b = (b * 255 + 15) / 31;
    return 0xff000000 | (r << 16) | (g << 8) | b;
}

static void emit_src565(CmdBuf *b, uint32_t origin)
{
    emit_state(b, DE_SRC_ADDRESS, SRC_PA);
    emit_state(b, DE_SRC_STRIDE, SW * 2);
    emit_state(b, DE_SRC_ROTATION_CONFIG, SW);
    emit_state(b, DE_SRC_CONFIG, FMT_R5G6B5 << 24);
    emit_state(b, DE_SRC_ORIGIN, origin);
}

static void test_2d_blit_convert(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, RAM + 0x10000, 0 };
    int x, y;

    fill_src565(qts);
    clear_mem(qts, DST_PA, 0);
    emit_dest(&b, DST_PA, CMD_BIT_BLT);
    emit_src565(&b, (1 << 16) | 2);
    emit_state(&b, DE_ROP, ROP3 | 0xcc);
    emit_rect(&b, 8, 4, 20, 12);
    run_2d(qts, &b);

    for (y = 0; y < SH; y++) {
        for (x = 0; x < SW; x++) {
            bool in = x >= 8 && x < 20 && y >= 4 && y < 12;
            uint32_t want = in ? argb_from565(x - 8 + 2, y - 4 + 1) : 0;
            g_assert_cmphex(px(qts, DST_PA, x, y), ==, want);
        }
    }
    qtest_quit(qts);
}

static void test_2d_rop_and_blend(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, RAM + 0x10000, 0 };
    int x;

    /* SRCINVERT (0x66, S xor D) on ARGB8888. */
    for (x = 0; x < SW * SH; x++) {
        qtest_writel(qts, SRC_PA + x * 4, 0x00ff00ff);
    }
    clear_mem(qts, DST_PA, 0xff0f0f0f);
    emit_dest(&b, DST_PA, CMD_BIT_BLT);
    emit_state(&b, DE_SRC_ADDRESS, SRC_PA);
    emit_state(&b, DE_SRC_STRIDE, SW * 4);
    emit_state(&b, DE_SRC_CONFIG, FMT_A8R8G8B8 << 24);
    emit_state(&b, DE_SRC_ORIGIN, 0);
    emit_state(&b, DE_ROP, ROP3 | 0x66);
    emit_rect(&b, 0, 0, 4, 1);
    run_2d(qts, &b);
    g_assert_cmphex(px(qts, DST_PA, 0, 0), ==, 0xfff00ff0);
    g_assert_cmphex(px(qts, DST_PA, 4, 0), ==, 0xff0f0f0f);

    /*
     * SRC_OVER with a 50% red source over opaque blue: source factor ONE,
     * destination factor INVERSED (1 - As).
     */
    for (x = 0; x < SW * SH; x++) {
        qtest_writel(qts, SRC_PA + x * 4, 0x80800000);
    }
    clear_mem(qts, DST_PA, 0xff0000ff);
    b.pos = 0;
    emit_dest(&b, DST_PA, CMD_BIT_BLT);
    emit_state(&b, DE_ROP, ROP3 | 0xcc);
    emit_state(&b, DE_ALPHA_CONTROL, 1);
    emit_state(&b, DE_ALPHA_MODES, (1u << 24) | (3u << 28));
    emit_rect(&b, 0, 0, 2, 2);
    run_2d(qts, &b);
    g_assert_cmphex(px(qts, DST_PA, 1, 1), ==, 0xff80007f);
    g_assert_cmphex(px(qts, DST_PA, 2, 1), ==, 0xff0000ff);
    qtest_quit(qts);
}

static void test_2d_stretch_and_rotate(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, RAM + 0x10000, 0 };
    int x, y;

    fill_src565(qts);
    clear_mem(qts, DST_PA, 0);

    /* 2x upscale of the 8x4 source block at (0,0) into 16x8. */
    emit_dest(&b, DST_PA, CMD_STRETCH_BLT);
    emit_src565(&b, 0);
    emit_state(&b, DE_ROP, ROP3 | 0xcc);
    emit_state(&b, DE_STRETCH_FACTOR_LOW, 1u << 15);
    emit_state(&b, DE_STRETCH_FACTOR_HIGH, 1u << 15);
    emit_rect(&b, 0, 0, 16, 8);
    run_2d(qts, &b);
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 16; x++) {
            g_assert_cmphex(px(qts, DST_PA, x, y), ==,
                            argb_from565(x / 2, y / 2));
        }
    }

    /*
     * Destination ROT180 on a SW x SH surface: logical (x, y) lands at
     * (SW - 1 - x, SH - 1 - y).
     */
    clear_mem(qts, DST_PA, 0);
    b.pos = 0;
    emit_dest(&b, DST_PA, CMD_BIT_BLT);
    emit_src565(&b, 0);
    emit_state(&b, DE_ROP, ROP3 | 0xcc);
    emit_state(&b, 0x12b4, SH);                 /* DEST_ROTATION_HEIGHT */
    emit_state(&b, DE_ROT_ANGLE, (5u << 3) | (1u << 8));
    emit_rect(&b, 0, 0, 4, 2);
    run_2d(qts, &b);
    for (y = 0; y < 2; y++) {
        for (x = 0; x < 4; x++) {
            g_assert_cmphex(px(qts, DST_PA, SW - 1 - x, SH - 1 - y), ==,
                            argb_from565(x, y));
        }
    }
    g_assert_cmphex(px(qts, DST_PA, 0, 0), ==, 0);
    qtest_quit(qts);
}

static void test_2d_through_mmu(void)
{
    QTestState *qts = start();
    CmdBuf b = { qts, RAM + 0x10000, 0 };
    int x;

    mmu_setup(qts, GPU2D, 64);
    /* The clear writes to GPU VA MAPPED_VA, which is RAM + 0x300000. */
    for (x = 0; x < SW; x++) {
        qtest_writel(qts, MAPPED_PA + x * 4, 0);
    }
    /* Command buffer also through the MMU: put it in the mapped area. */
    b.base = MAPPED_PA + 0x8000;
    emit_dest(&b, MAPPED_VA, CMD_CLEAR);
    emit_state(&b, DE_ROP, ROP3 | 0xcc);
    emit_state(&b, DE_CLEAR_PIXEL_VALUE32, 0xffabcdef);
    emit_rect(&b, 0, 0, 8, 1);
    emit_event(&b, 0, EVENT_FROM_PE);
    emit_end(&b);
    fe_start(qts, GPU2D, MAPPED_VA + 0x8000, b.pos);
    qtest_clock_step(qts, STEP);
    g_assert_cmphex(rd(qts, GPU2D, INTR_ACK), ==, 1);
    g_assert_cmphex(qtest_readl(qts, MAPPED_PA + 7 * 4), ==, 0xffabcdef);
    g_assert_cmphex(qtest_readl(qts, MAPPED_PA + 8 * 4), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/gpu/ids", test_ids);
    qtest_add_func("/kick/gpu/reset_sequence", test_reset_sequence);
    qtest_add_func("/kick/gpu/fe_events", test_fe_events);
    qtest_add_func("/kick/gpu/mmu", test_mmu);
    qtest_add_func("/kick/gpu/3d_draw_completes", test_3d_draw_completes);
    qtest_add_func("/kick/gpu/2d_clear", test_2d_clear);
    qtest_add_func("/kick/gpu/2d_blit_convert", test_2d_blit_convert);
    qtest_add_func("/kick/gpu/2d_rop_and_blend", test_2d_rop_and_blend);
    qtest_add_func("/kick/gpu/2d_stretch_and_rotate",
                   test_2d_stretch_and_rotate);
    qtest_add_func("/kick/gpu/2d_through_mmu", test_2d_through_mmu);

    return g_test_run();
}
