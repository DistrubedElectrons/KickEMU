/*
 * qtest for the i.MX8MM UART model (UART1 of the kick machine)
 *
 * Covers the register behaviour the Linux imx serial driver depends on:
 * reset values, software reset, TX/RX FIFOs with line timing, RX/TX
 * thresholds, ageing timer, idle detect, the SDMA request lines, and the
 * SDMA1 uart_2_mcu / mcu_2_app scripts on events 22/23 as the driver sets
 * them up (cyclic RX with ageing close, chained TX BDs).
 *
 * UART1 is connected to a unix socket served by the test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include "libqtest.h"

#define UART            0x30860000
#define UART_PATH       "/machine/soc/uart1"
#define SDMA            0x30bd0000

#define URXD            0x00
#define UTXD            0x40
#define UCR1            0x80
#define UCR2            0x84
#define UCR3            0x88
#define UCR4            0x8c
#define UFCR            0x90
#define USR1            0x94
#define USR2            0x98
#define UESC            0x9c
#define UBIR            0xa4
#define UBMR            0xa8
#define UBRC            0xac
#define UTS             0xb4

#define URXD_CHARRDY    (1u << 15)
#define URXD_ERR        (1u << 14)
#define URXD_OVRRUN     (1u << 13)

#define UCR1_IDEN       (1u << 12)
#define UCR1_RXDMAEN    (1u << 8)
#define UCR1_TXDMAEN    (1u << 3)
#define UCR1_ATDMAEN    (1u << 2)
#define UCR1_UARTEN     (1u << 0)

#define UCR2_IRTS       (1u << 14)
#define UCR2_WS         (1u << 5)
#define UCR2_TXEN       (1u << 2)
#define UCR2_RXEN       (1u << 1)
#define UCR2_SRST       (1u << 0)

#define USR1_TRDY       (1u << 13)
#define USR1_RRDY       (1u << 9)
#define USR1_AGTIM      (1u << 8)

#define USR2_TXFE       (1u << 14)
#define USR2_IDLE       (1u << 12)
#define USR2_TXDC       (1u << 3)
#define USR2_ORE        (1u << 1)
#define USR2_RDR        (1u << 0)

#define UTS_TXEMPTY     (1u << 6)
#define UTS_RXEMPTY     (1u << 5)
#define UTS_RXFULL      (1u << 3)

/*
 * Line setup used by the tests: 24 MHz UART1 root, RFDIV /2, UBIR 9,
 * UBMR 99: 75000 baud, 8N1 = 10 bits per frame.
 */
#define UFCR_RFDIV2     (4u << 7)
#define FRAME_NS        133333

/* SDMA */
#define MC0PTR          0x000
#define INTR            0x004
#define STOP_STAT       0x008
#define HSTART          0x00c
#define EVTOVR          0x010
#define DSPOVR          0x014
#define HOSTOVR         0x018
#define EVTERR          0x028
#define CONFIG          0x038
#define CHN0ADDR        0x05c
#define CHNPRI(n)       (0x100 + 4 * (n))
#define CHNENBL(n)      (0x200 + 4 * (n))

#define BD_DONE         0x01
#define BD_WRAP         0x02
#define BD_CONT         0x04
#define BD_INTR         0x08
#define BD_RROR         0x10
#define BD_LAST         0x20
#define BD_EXTD         0x80

#define C0_SETDM        0x01

/* i.MX 7D/8M SDMA ROM scripts */
#define MCU_2_APP       749
#define UART_2_MCU      819

#define EV_UART1_RX     22
#define EV_UART1_TX     23

#define RAM             0x40000000
#define CCB             RAM
#define CTX             (CCB + 32 * 16)
#define BD0             (RAM + 0x1000)
#define BD_RX           (RAM + 0x1100)
#define BD_TX           (RAM + 0x1200)
#define BUF_RX          (RAM + 0x2000)
#define BUF_TX          (RAM + 0x3000)
#define CTX_WORDS       32

typedef struct {
    QTestState *qts;
    int sock;
    char *dir;
    char *path;
} Fixture;

static void fixture_init(Fixture *f)
{
    int srv;

    f->dir = g_dir_make_tmp("kick-uart-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->path = g_build_filename(f->dir, "u.sock", NULL);
    srv = qtest_socket_server(f->path);
    f->qts = qtest_initf("-M kick -serial unix:%s", f->path);
    f->sock = accept(srv, NULL, NULL);
    g_assert_cmpint(f->sock, >=, 0);
    close(srv);
}

static void fixture_done(Fixture *f)
{
    qtest_quit(f->qts);
    close(f->sock);
    unlink(f->path);
    rmdir(f->dir);
    g_free(f->path);
    g_free(f->dir);
}

static uint32_t rd(Fixture *f, uint32_t off)
{
    return qtest_readl(f->qts, UART + off);
}

static void wr(Fixture *f, uint32_t off, uint32_t val)
{
    qtest_writel(f->qts, UART + off, val);
}

static void step_frames(Fixture *f, double frames)
{
    qtest_clock_step(f->qts, (int64_t)(frames * FRAME_NS));
}

/* imx_uart_startup() and set_termios() for 8N1 at the test baud rate */
static void uart_setup(Fixture *f, unsigned rxtl, unsigned txtl)
{
    wr(f, UCR2, rd(f, UCR2) & ~UCR2_SRST);
    g_assert_cmphex(rd(f, UCR2) & UCR2_SRST, ==, UCR2_SRST);
    wr(f, UFCR, (txtl << 10) | UFCR_RFDIV2 | rxtl);
    wr(f, UBIR, 9);
    wr(f, UBMR, 99);
    wr(f, UCR1, UCR1_UARTEN);
    wr(f, UCR2, UCR2_IRTS | UCR2_WS | UCR2_TXEN | UCR2_RXEN | UCR2_SRST);
}

static void sock_send(Fixture *f, const void *buf, size_t len)
{
    g_assert_cmpint(send(f->sock, buf, len, 0), ==, len);
}

/* Read exactly len bytes the UART sent, failing after a few seconds */
static void sock_recv(Fixture *f, uint8_t *buf, size_t len)
{
    struct timeval tv = { .tv_sec = 5 };
    size_t got = 0;

    setsockopt(f->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (got < len) {
        ssize_t n = recv(f->sock, buf + got, len - got, 0);

        g_assert_cmpint(n, >, 0);
        got += n;
    }
}

/*
 * Advance in small steps until (reg & mask) == want. Each qtest command
 * also lets the main loop hand the next character from the socket to the
 * UART, so this is how the tests wait for input.
 */
static bool wait_reg(Fixture *f, uint32_t off, uint32_t mask, uint32_t want)
{
    for (int i = 0; i < 4000; i++) {
        if ((rd(f, off) & mask) == want) {
            return true;
        }
        step_frames(f, 0.125);
    }
    return false;
}

static void test_reset_values(void)
{
    Fixture f;

    fixture_init(&f);
    /* UARTEN/TXEN as the boot ROM leaves them */
    g_assert_cmphex(rd(&f, UCR1), ==, UCR1_UARTEN);
    g_assert_cmphex(rd(&f, UCR2), ==, UCR2_TXEN | UCR2_SRST);
    g_assert_cmphex(rd(&f, UCR3), ==, 0x0700);
    g_assert_cmphex(rd(&f, UCR4), ==, 0x8000);
    g_assert_cmphex(rd(&f, UFCR), ==, 0x0801);
    g_assert_cmphex(rd(&f, USR1), ==, 0x6040);
    g_assert_cmphex(rd(&f, USR2), ==, 0x4028);
    g_assert_cmphex(rd(&f, UESC), ==, 0x2b);
    g_assert_cmphex(rd(&f, UBRC), ==, 0x4);
    g_assert_cmphex(rd(&f, UTS), ==, UTS_TXEMPTY | UTS_RXEMPTY);
    fixture_done(&f);
}

/* SRST clears FIFOs, USR1/2, UBIR, UBMR; the control registers stay */
static void test_soft_reset(void)
{
    Fixture f;
    uint8_t c;

    fixture_init(&f);
    uart_setup(&f, 1, 2);
    wr(&f, UCR3, 0x0704);
    sock_send(&f, "q", 1);
    g_assert_true(wait_reg(&f, USR2, USR2_RDR, USR2_RDR));
    wr(&f, UTXD, 'z');
    sock_recv(&f, &c, 1);
    g_assert_cmphex(rd(&f, USR2) & USR2_TXDC, ==, 0);

    wr(&f, UCR2, UCR2_WS | UCR2_TXEN | UCR2_RXEN);
    g_assert_cmphex(rd(&f, UCR2), ==, UCR2_WS | UCR2_TXEN | UCR2_RXEN |
                    UCR2_SRST);
    g_assert_cmphex(rd(&f, UBIR), ==, 0);
    g_assert_cmphex(rd(&f, UBMR), ==, 0);
    g_assert_cmphex(rd(&f, USR1), ==, 0x6040);
    g_assert_cmphex(rd(&f, USR2), ==, 0x4028);
    g_assert_cmphex(rd(&f, UTS), ==, UTS_TXEMPTY | UTS_RXEMPTY);
    g_assert_cmphex(rd(&f, UCR1), ==, UCR1_UARTEN);
    g_assert_cmphex(rd(&f, UCR3), ==, 0x0704);
    g_assert_cmphex(rd(&f, UFCR), ==, (2u << 10) | UFCR_RFDIV2 | 1);
    fixture_done(&f);
}

/* TX FIFO drains one character per frame; TRDY at TXTL, TXDC at the end */
static void test_tx_fifo(void)
{
    Fixture f;
    uint8_t buf[6];

    fixture_init(&f);
    uart_setup(&f, 1, 2);
    for (int i = 0; i < 6; i++) {
        wr(&f, UTXD, 'a' + i);
    }
    /* 'a' is in the shifter, five wait in the FIFO */
    g_assert_cmphex(rd(&f, USR1) & USR1_TRDY, ==, 0);
    g_assert_cmphex(rd(&f, USR2) & (USR2_TXFE | USR2_TXDC), ==, 0);
    g_assert_cmphex(rd(&f, UTS) & UTS_TXEMPTY, ==, 0);

    step_frames(&f, 2.5);       /* 'c' on the line, 3 left */
    g_assert_cmphex(rd(&f, USR1) & USR1_TRDY, ==, 0);
    step_frames(&f, 1);         /* 2 left: TXTL or fewer */
    g_assert_cmphex(rd(&f, USR1) & USR1_TRDY, ==, USR1_TRDY);
    step_frames(&f, 2);         /* 'f' on the line */
    g_assert_cmphex(rd(&f, USR2) & (USR2_TXFE | USR2_TXDC), ==, USR2_TXFE);
    step_frames(&f, 1);
    g_assert_cmphex(rd(&f, USR2) & (USR2_TXFE | USR2_TXDC), ==,
                    USR2_TXFE | USR2_TXDC);

    sock_recv(&f, buf, sizeof(buf));
    g_assert_cmpmem(buf, sizeof(buf), "abcdef", 6);
    fixture_done(&f);
}

/* TX DMA request: TXDMAEN and TRDY */
static void test_tx_dma_request(void)
{
    Fixture f;

    fixture_init(&f);
    qtest_irq_intercept_out_named(f.qts, UART_PATH, "dma-tx");
    uart_setup(&f, 1, 2);
    g_assert_false(qtest_get_irq(f.qts, 0));
    wr(&f, UCR1, UCR1_UARTEN | UCR1_TXDMAEN);
    g_assert_true(qtest_get_irq(f.qts, 0));
    for (int i = 0; i < 4; i++) {
        wr(&f, UTXD, '0' + i);
    }
    g_assert_false(qtest_get_irq(f.qts, 0));    /* 3 in the FIFO */
    step_frames(&f, 1);
    g_assert_true(qtest_get_irq(f.qts, 0));
    fixture_done(&f);
}

/* RX: one character per frame, the next waits in the socket meanwhile */
static void test_rx_timing(void)
{
    Fixture f;

    fixture_init(&f);
    uart_setup(&f, 1, 2);
    sock_send(&f, "xyz", 3);
    for (int i = 0; i < 3; i++) {
        g_assert_true(wait_reg(&f, USR2, USR2_RDR, USR2_RDR));
        g_assert_cmphex(rd(&f, URXD), ==, URXD_CHARRDY | ("xyz")[i]);
        /* No time has passed: the next character is still on the line */
        g_assert_cmphex(rd(&f, USR2) & USR2_RDR, ==, 0);
        g_assert_cmphex(rd(&f, URXD) & URXD_CHARRDY, ==, 0);
    }
    fixture_done(&f);
}

/* A full RX FIFO holds the line: OVRRUN marks entry 32, nothing is lost */
static void test_rx_fifo_full(void)
{
    Fixture f;
    uint8_t buf[40];

    fixture_init(&f);
    uart_setup(&f, 1, 2);
    for (int i = 0; i < sizeof(buf); i++) {
        buf[i] = 0x40 + i;
    }
    sock_send(&f, buf, sizeof(buf));
    g_assert_true(wait_reg(&f, UTS, UTS_RXFULL, UTS_RXFULL));
    step_frames(&f, 20);
    for (int i = 0; i < 32; i++) {
        uint32_t v = rd(&f, URXD);
        uint32_t flags = i == 31 ? URXD_ERR | URXD_OVRRUN : 0;

        g_assert_cmphex(v, ==, URXD_CHARRDY | flags | buf[i]);
    }
    g_assert_cmphex(rd(&f, USR2) & USR2_ORE, ==, 0);
    for (int i = 32; i < sizeof(buf); i++) {
        g_assert_true(wait_reg(&f, USR2, USR2_RDR, USR2_RDR));
        g_assert_cmphex(rd(&f, URXD), ==, URXD_CHARRDY | buf[i]);
    }
    fixture_done(&f);
}

/* RX DMA request: RRDY at RXTL, ageing timer after 8 frames below RXTL */
static void test_rx_dma_request_ageing(void)
{
    Fixture f;

    fixture_init(&f);
    qtest_irq_intercept_out_named(f.qts, UART_PATH, "dma-rx");
    uart_setup(&f, 4, 2);
    wr(&f, UCR1, UCR1_UARTEN | UCR1_RXDMAEN | UCR1_ATDMAEN);

    sock_send(&f, "abc", 3);
    g_assert_true(wait_reg(&f, USR1, USR1_AGTIM, USR1_AGTIM));
    g_assert_cmphex(rd(&f, USR1) & USR1_RRDY, ==, 0);
    g_assert_true(qtest_get_irq(f.qts, 0));

    /* A FIFO read restarts the timer; AGTIM is write-1-to-clear */
    g_assert_cmphex(rd(&f, URXD), ==, URXD_CHARRDY | 'a');
    wr(&f, USR1, USR1_AGTIM);
    g_assert_false(qtest_get_irq(f.qts, 0));
    step_frames(&f, 7.5);
    g_assert_cmphex(rd(&f, USR1) & USR1_AGTIM, ==, 0);
    step_frames(&f, 1);
    g_assert_cmphex(rd(&f, USR1) & USR1_AGTIM, ==, USR1_AGTIM);
    wr(&f, USR1, USR1_AGTIM);

    /* Two more make four: RRDY and the request, no ageing at RXTL */
    sock_send(&f, "de", 2);
    g_assert_true(wait_reg(&f, USR1, USR1_RRDY, USR1_RRDY));
    g_assert_true(qtest_get_irq(f.qts, 0));
    step_frames(&f, 20);
    g_assert_cmphex(rd(&f, USR1) & USR1_AGTIM, ==, 0);
    g_assert_cmphex(rd(&f, URXD), ==, URXD_CHARRDY | 'b');
    g_assert_false(qtest_get_irq(f.qts, 0));
    fixture_done(&f);
}

/* Idle detect: USR2.IDLE after more than 4 idle frames (ICD = 0) */
static void test_idle(void)
{
    Fixture f;

    fixture_init(&f);
    qtest_irq_intercept_out_named(f.qts, UART_PATH, "irq");
    uart_setup(&f, 1, 2);
    wr(&f, UCR1, UCR1_UARTEN | UCR1_IDEN);

    sock_send(&f, "i", 1);
    g_assert_true(wait_reg(&f, USR2, USR2_RDR, USR2_RDR));
    step_frames(&f, 3.5);
    g_assert_cmphex(rd(&f, USR2) & USR2_IDLE, ==, 0);
    g_assert_false(qtest_get_irq(f.qts, 0));
    step_frames(&f, 1);
    g_assert_cmphex(rd(&f, USR2) & USR2_IDLE, ==, USR2_IDLE);
    g_assert_true(qtest_get_irq(f.qts, 0));
    wr(&f, USR2, USR2_IDLE);
    g_assert_false(qtest_get_irq(f.qts, 0));
    /* Only once per idle period */
    step_frames(&f, 50);
    g_assert_cmphex(rd(&f, USR2) & USR2_IDLE, ==, 0);
    fixture_done(&f);
}

/* SDMA1 setup as sdma_runtime_resume() and sdma_request_channel0() */
static void sdma_wr(Fixture *f, uint32_t off, uint32_t val)
{
    qtest_writel(f->qts, SDMA + off, val);
}

static uint32_t sdma_rd(Fixture *f, uint32_t off)
{
    return qtest_readl(f->qts, SDMA + off);
}

static void write_bd(Fixture *f, uint32_t addr, uint16_t count,
                     uint8_t status, uint8_t cmd, uint32_t buf, uint32_t ext)
{
    qtest_writel(f->qts, addr, count | (status << 16) | ((uint32_t)cmd << 24));
    qtest_writel(f->qts, addr + 4, buf);
    qtest_writel(f->qts, addr + 8, ext);
}

static uint32_t bd_mode(Fixture *f, uint32_t addr)
{
    return qtest_readl(f->qts, addr);
}

/* Advance until the SDMA hands the BD back (DONE cleared) */
static bool wait_bd_closed(Fixture *f, uint32_t addr)
{
    for (int i = 0; i < 4000; i++) {
        if (!(bd_mode(f, addr) & (BD_DONE << 16))) {
            return true;
        }
        step_frames(f, 0.5);
    }
    return false;
}

static void sdma_init(Fixture *f)
{
    sdma_wr(f, MC0PTR, 0);
    qtest_writel(f->qts, CCB + 0, BD0);
    qtest_writel(f->qts, CCB + 4, BD0);
    sdma_wr(f, EVTOVR, 1);
    sdma_wr(f, DSPOVR, 0xffffffff);
    sdma_wr(f, CHN0ADDR, 0x4050);
    sdma_wr(f, CONFIG, 0);
    sdma_wr(f, MC0PTR, CCB);
    sdma_wr(f, CHNPRI(0), 7);
}

/* sdma_load_context(): PC, gReg[1] = event mask, gReg[6] FIFO, gReg[7] wml */
static void sdma_channel(Fixture *f, int ch, int event, uint32_t pc,
                         uint32_t fifo, uint32_t wml, uint32_t bd)
{
    for (int i = 0; i < CTX_WORDS; i++) {
        qtest_writel(f->qts, CTX + i * 4, 0);
    }
    qtest_writel(f->qts, CTX, pc);
    qtest_writel(f->qts, CTX + 8 + 1 * 4, 1u << event);
    qtest_writel(f->qts, CTX + 8 + 6 * 4, fifo);
    qtest_writel(f->qts, CTX + 8 + 7 * 4, wml);
    write_bd(f, BD0, CTX_WORDS, BD_DONE | BD_WRAP | BD_EXTD, C0_SETDM, CTX,
             2048 + CTX_WORDS * ch);
    sdma_wr(f, HSTART, 1);

    sdma_wr(f, CHNENBL(event), sdma_rd(f, CHNENBL(event)) | (1u << ch));
    sdma_wr(f, CHNPRI(ch), 4);
    qtest_writel(f->qts, CCB + ch * 16, bd);
    qtest_writel(f->qts, CCB + ch * 16 + 4, bd);
}

/*
 * imx_uart_start_rx_dma(): cyclic uart_2_mcu over 4 BDs, RXTL 9, burst 8.
 * 37 characters: two full BDs, the last 5 close BD2 through the ageing
 * timer, the script clears AGTIM and the request drops.
 */
static void test_sdma_rx(void)
{
    Fixture f;
    uint8_t buf[37];
    const int period = 16;

    fixture_init(&f);
    for (int i = 0; i < sizeof(buf); i++) {
        buf[i] = g_test_rand_int_range(0, 256);
    }
    sdma_init(&f);
    for (int i = 0; i < 4; i++) {
        write_bd(&f, BD_RX + i * 12, period,
                 BD_DONE | BD_EXTD | BD_CONT | BD_INTR | (i == 3 ? BD_WRAP : 0),
                 1, BUF_RX + i * period, 0);
    }
    sdma_channel(&f, 1, EV_UART1_RX, UART_2_MCU, UART + URXD, 8, BD_RX);
    uart_setup(&f, 9, 8);
    wr(&f, UCR1, UCR1_UARTEN | UCR1_RXDMAEN | UCR1_ATDMAEN);
    sdma_wr(&f, HSTART, 1 << 1);

    sock_send(&f, buf, sizeof(buf));
    g_assert_true(wait_bd_closed(&f, BD_RX + 24));
    g_assert_cmphex(bd_mode(&f, BD_RX) >> 16 & (BD_DONE | BD_RROR), ==, 0);
    g_assert_cmpuint(bd_mode(&f, BD_RX) & 0xffff, ==, period);
    g_assert_cmphex(bd_mode(&f, BD_RX + 12) >> 16 & (BD_DONE | BD_RROR), ==, 0);
    g_assert_cmpuint(bd_mode(&f, BD_RX + 12) & 0xffff, ==, period);
    g_assert_cmphex(bd_mode(&f, BD_RX + 24) >> 16 & (BD_DONE | BD_RROR), ==, 0);
    g_assert_cmpuint(bd_mode(&f, BD_RX + 24) & 0xffff, ==, 5);
    g_assert_cmphex(bd_mode(&f, BD_RX + 36) >> 16 & BD_DONE, ==, BD_DONE);
    for (int i = 0; i < sizeof(buf); i++) {
        g_assert_cmphex(qtest_readb(f.qts, BUF_RX + i), ==, buf[i]);
    }
    g_assert_cmphex(sdma_rd(&f, INTR) & (1 << 1), ==, 1 << 1);
    g_assert_cmphex(rd(&f, USR1) & (USR1_AGTIM | USR1_RRDY), ==, 0);
    g_assert_cmphex(rd(&f, UTS) & UTS_RXEMPTY, ==, UTS_RXEMPTY);
    g_assert_cmphex(sdma_rd(&f, EVTERR), ==, 0);
    /* The channel keeps running: more data goes to BD3 */
    sock_send(&f, "+", 1);
    g_assert_true(wait_bd_closed(&f, BD_RX + 36));
    g_assert_cmphex(bd_mode(&f, BD_RX + 36), ==,
                    ((BD_EXTD | BD_CONT | BD_INTR | BD_WRAP) << 16) |
                    (1u << 24) | 1);
    g_assert_cmphex(qtest_readb(f.qts, BUF_RX + 3 * period), ==, '+');
    fixture_done(&f);
}

/*
 * imx_uart_dma_tx() with a wrapped circular buffer: two BDs, only the last
 * one interrupts. mcu_2_app writes bursts of 8 whenever TRDY (TXTL 8).
 */
static void test_sdma_tx(void)
{
    Fixture f;
    uint8_t buf[25], out[25];

    fixture_init(&f);
    for (int i = 0; i < sizeof(buf); i++) {
        buf[i] = g_test_rand_int_range(0, 256);
        qtest_writeb(f.qts, BUF_TX + i, buf[i]);
    }
    sdma_init(&f);
    write_bd(&f, BD_TX, 5, BD_DONE | BD_EXTD | BD_CONT, 1, BUF_TX, 0);
    write_bd(&f, BD_TX + 12, 20, BD_DONE | BD_EXTD | BD_INTR | BD_LAST, 1,
             BUF_TX + 5, 0);
    sdma_channel(&f, 2, EV_UART1_TX, MCU_2_APP, UART + UTXD, 8, BD_TX);
    uart_setup(&f, 9, 8);
    wr(&f, UCR1, UCR1_UARTEN | UCR1_TXDMAEN);
    sdma_wr(&f, HSTART, 1 << 2);

    for (int i = 0; i < 400 && !(sdma_rd(&f, INTR) & (1 << 2)); i++) {
        step_frames(&f, 0.5);
    }
    g_assert_cmphex(sdma_rd(&f, INTR) & (1 << 2), ==, 1 << 2);
    g_assert_cmphex(bd_mode(&f, BD_TX) >> 16 & (BD_DONE | BD_RROR), ==, 0);
    g_assert_cmphex(bd_mode(&f, BD_TX + 12) >> 16 & (BD_DONE | BD_RROR), ==, 0);
    g_assert_cmphex(sdma_rd(&f, STOP_STAT) & (1 << 2), ==, 0);
    /* The SDMA is done once the last byte is in the FIFO, not on the line */
    g_assert_cmphex(rd(&f, USR2) & USR2_TXDC, ==, 0);
    g_assert_true(wait_reg(&f, USR2, USR2_TXDC, USR2_TXDC));
    sock_recv(&f, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), buf, sizeof(buf));
    g_assert_cmphex(sdma_rd(&f, EVTERR), ==, 0);
    fixture_done(&f);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/uart/reset-values", test_reset_values);
    qtest_add_func("/kick/uart/soft-reset", test_soft_reset);
    qtest_add_func("/kick/uart/tx-fifo", test_tx_fifo);
    qtest_add_func("/kick/uart/tx-dma-request", test_tx_dma_request);
    qtest_add_func("/kick/uart/rx-timing", test_rx_timing);
    qtest_add_func("/kick/uart/rx-fifo-full", test_rx_fifo_full);
    qtest_add_func("/kick/uart/rx-dma-request-ageing",
                   test_rx_dma_request_ageing);
    qtest_add_func("/kick/uart/idle", test_idle);
    qtest_add_func("/kick/uart/sdma-rx", test_sdma_rx);
    qtest_add_func("/kick/uart/sdma-tx", test_sdma_tx);
    return g_test_run();
}
