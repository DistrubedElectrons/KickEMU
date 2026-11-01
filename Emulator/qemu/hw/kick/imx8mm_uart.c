/*
 * i.MX 8M Mini UART
 *
 * Model of the UART as described in the i.MX 8M Mini Reference Manual,
 * chapter "Universal Asynchronous Receiver/Transmitter (UART)", written for
 * the Linux imx serial driver in both PIO and SDMA mode. QEMU's imx_serial
 * has no TX FIFO, no line timing and writes the backend synchronously, which
 * the DMA mode of the driver cannot work with: the TX request never drops,
 * a blocked backend stalls the whole main loop, and received characters
 * arrive 32 at a time so the RX FIFO overruns before the SDMA can drain it.
 *
 * Modelled:
 *  - 32 entry TX and RX FIFOs. TRDY while the TX FIFO holds TXTL or fewer
 *    characters, RRDY while the RX FIFO holds RXTL or more; TXFE, TXDC,
 *    RDR and the UTS FIFO flags follow the FIFO and shifter state.
 *  - Line timing: one character leaves the TX shifter and one enters the RX
 *    shifter per frame time, from the reference clock, RFDIV, UBIR/UBMR and
 *    the frame format in UCR2 (start, 7/8 data, parity, 1/2 stop bits). The
 *    reference clock is the UARTn CCM root when the "ccm" link is set.
 *  - Ageing timer: AGTIM after 8 frame times without a character received
 *    or read while the RX FIFO is not empty and below RXTL. Idle detect:
 *    USR2.IDLE after 4/8/16/32 idle frames (UCR1.ICD) following a character.
 *  - DMA requests to the SDMA:
 *      dma_req_rx = RXDMAEN & RRDY | ATDMAEN & AGTIM | IDDMAEN & IDLE
 *      dma_req_tx = TXDMAEN & TRDY
 *  - The interrupt output: every USR1/USR2 source with its enable bit in
 *    UCR1..UCR4.
 *  - Software reset (UCR2.SRST = 0) clears the FIFOs, the state machines,
 *    USR1, USR2, UBIR, UBMR and the UTS status bits, nothing else.
 *  - UTS.LOOP: the transmitter feeds the receiver instead of the backend.
 *  - Backend flow control: the transmitter waits while the backend cannot
 *    take a character, the receiver accepts the next character only when
 *    the RX FIFO has room. Nothing is lost on either side.
 *
 * Not modelled (registers are stored): autobaud, escape detection, IrDA,
 * RS-485 (UMCR), DTE mode pins, RTS/DTR/DCD/RI changes (RTS_B and DCD_B
 * read as asserted), break transmission (UCR1.SNDBRK).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"

#include "imx8mm_uart.h"

/* Register offsets */
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
#define UTIM            0xa0
#define UBIR            0xa4
#define UBMR            0xa8
#define UBRC            0xac
#define ONEMS           0xb0
#define UTS             0xb4
#define UMCR            0xb8

#define URXD_CHARRDY    BIT(15)
#define URXD_ERR        BIT(14)
#define URXD_OVRRUN     BIT(13)
#define URXD_FRMERR     BIT(12)
#define URXD_BRK        BIT(11)

#define UCR1_ADEN       BIT(15)
#define UCR1_TRDYEN     BIT(13)
#define UCR1_IDEN       BIT(12)
#define UCR1_ICD_SHIFT  10
#define UCR1_RRDYEN     BIT(9)
#define UCR1_RXDMAEN    BIT(8)
#define UCR1_TXMPTYEN   BIT(6)
#define UCR1_RTSDEN     BIT(5)
#define UCR1_TXDMAEN    BIT(3)
#define UCR1_ATDMAEN    BIT(2)
#define UCR1_UARTEN     BIT(0)

#define UCR2_ESCI       BIT(15)
#define UCR2_PREN       BIT(8)
#define UCR2_STPB       BIT(6)
#define UCR2_WS         BIT(5)
#define UCR2_RTSEN      BIT(4)
#define UCR2_ATEN       BIT(3)
#define UCR2_TXEN       BIT(2)
#define UCR2_RXEN       BIT(1)
#define UCR2_SRST       BIT(0)

#define UCR3_DTREN      BIT(13)
#define UCR3_PARERREN   BIT(12)
#define UCR3_FRAERREN   BIT(11)
#define UCR3_RXDSEN     BIT(6)
#define UCR3_AIRINTEN   BIT(5)
#define UCR3_AWAKEN     BIT(4)
#define UCR3_DTRDEN     BIT(3)

#define UCR4_ENIRI      BIT(8)
#define UCR4_WKEN       BIT(7)
#define UCR4_IDDMAEN    BIT(6)
#define UCR4_TCEN       BIT(3)
#define UCR4_BKEN       BIT(2)
#define UCR4_OREN       BIT(1)
#define UCR4_DREN       BIT(0)

#define UFCR_TXTL_SHIFT 10
#define UFCR_RFDIV_SHIFT 7
#define UFCR_TL_MASK    0x3f

#define USR1_PARITYERR  BIT(15)
#define USR1_RTSS       BIT(14)
#define USR1_TRDY       BIT(13)
#define USR1_RTSD       BIT(12)
#define USR1_ESCF       BIT(11)
#define USR1_FRAMERR    BIT(10)
#define USR1_RRDY       BIT(9)
#define USR1_AGTIM      BIT(8)
#define USR1_DTRD       BIT(7)
#define USR1_RXDS       BIT(6)
#define USR1_AIRINT     BIT(5)
#define USR1_AWAKE      BIT(4)
#define USR1_SAD        BIT(3)
#define USR1_W1C        (USR1_PARITYERR | USR1_RTSD | USR1_ESCF | \
                         USR1_FRAMERR | USR1_AGTIM | USR1_DTRD | \
                         USR1_AIRINT | USR1_AWAKE | USR1_SAD)

#define USR2_ADET       BIT(15)
#define USR2_TXFE       BIT(14)
#define USR2_DTRF       BIT(13)
#define USR2_IDLE       BIT(12)
#define USR2_ACST       BIT(11)
#define USR2_RIDELT     BIT(10)
#define USR2_IRINT      BIT(8)
#define USR2_WAKE       BIT(7)
#define USR2_DCDDELT    BIT(6)
#define USR2_DCDIN      BIT(5)
#define USR2_RTSF       BIT(4)
#define USR2_TXDC       BIT(3)
#define USR2_BRCD       BIT(2)
#define USR2_ORE        BIT(1)
#define USR2_RDR        BIT(0)
#define USR2_W1C        (USR2_ADET | USR2_DTRF | USR2_IDLE | USR2_ACST | \
                         USR2_RIDELT | USR2_IRINT | USR2_WAKE | \
                         USR2_DCDDELT | USR2_RTSF | USR2_BRCD | USR2_ORE)

#define UTS_LOOP        BIT(12)
#define UTS_TXEMPTY     BIT(6)
#define UTS_RXEMPTY     BIT(5)
#define UTS_TXFULL      BIT(4)
#define UTS_RXFULL      BIT(3)
#define UTS_RW_MASK     0x3e00      /* FRCPERR, LOOP, DBGEN, LOOPIR, RXDBG */

/* Reset values (RM register descriptions) */
#define UCR3_RESET      0x0700
#define UCR4_RESET      0x8000      /* CTSTL = 32 */
#define UFCR_RESET      0x0801      /* TXTL = 2, RXTL = 1 */
#define USR1_RESET      (USR1_TRDY | USR1_RTSS | USR1_RXDS)
#define USR2_RESET      (USR2_TXFE | USR2_DCDIN | USR2_TXDC)
#define UESC_RESET      0x2b
#define UBRC_RESET      0x4
#define UTS_RESET       (UTS_TXEMPTY | UTS_RXEMPTY)

/* Frames without a received or read character before AGTIM */
#define AGEING_FRAMES   8

static bool uart_enabled(IMX8MMUARTState *s, uint32_t ucr2_bit)
{
    return (s->ucr1 & UCR1_UARTEN) && (s->ucr2 & ucr2_bit);
}

static uint64_t uart_ref_hz(IMX8MMUARTState *s)
{
    uint64_t hz = 0;

    if (s->ccm && s->ccm_root) {
        hz = imx8mm_ccm_root_freq(s->ccm, s->ccm_root);
    }
    return hz ? hz : s->ref_clock_hz;
}

/*
 * Duration of one frame: baud = ref / RFDIV / (16 * (UBMR + 1) / (UBIR + 1)),
 * frame = start bit + 7 or 8 data bits + optional parity + 1 or 2 stop bits.
 */
static int64_t uart_frame_ns(IMX8MMUARTState *s)
{
    static const uint8_t rfdiv[8] = { 6, 5, 4, 3, 2, 1, 7, 1 };
    uint64_t div = rfdiv[(s->ufcr >> UFCR_RFDIV_SHIFT) & 7] * 16ull *
                   (s->ubmr + 1);
    uint64_t mbaud = uart_ref_hz(s) * (s->ubir + 1) * 1000 / div;
    uint64_t bits = 1 + ((s->ucr2 & UCR2_WS) ? 8 : 7) +
                    ((s->ucr2 & UCR2_PREN) ? 1 : 0) +
                    ((s->ucr2 & UCR2_STPB) ? 2 : 1);

    /* Millibaud keeps the integer division exact enough at any rate */
    return bits * NANOSECONDS_PER_SECOND * 1000 / MAX(mbaud, 1);
}

static int64_t uart_after_frames(IMX8MMUARTState *s, unsigned frames)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + frames * uart_frame_ns(s);
}

/* Recompute the FIFO driven status bits, the interrupt and DMA requests */
static void uart_update(IMX8MMUARTState *s)
{
    uint32_t rxn = fifo32_num_used(&s->rx_fifo);
    uint32_t txn = fifo32_num_used(&s->tx_fifo);
    uint32_t rxtl = s->ufcr & UFCR_TL_MASK;
    uint32_t txtl = (s->ufcr >> UFCR_TXTL_SHIFT) & UFCR_TL_MASK;
    uint32_t u1, u2;
    bool irq, rx, tx;

    s->usr1 &= ~(USR1_TRDY | USR1_RRDY | USR1_RXDS);
    s->usr2 &= ~(USR2_TXFE | USR2_TXDC | USR2_RDR);
    s->uts &= ~(UTS_TXEMPTY | UTS_RXEMPTY | UTS_TXFULL | UTS_RXFULL);

    /* RM: TRDY when the TxFIFO "has TXTL or fewer characters" */
    if (txn <= txtl) {
        s->usr1 |= USR1_TRDY;
    }
    if (rxn && rxn >= rxtl) {
        s->usr1 |= USR1_RRDY;
    }
    if (!s->rx_busy) {
        s->usr1 |= USR1_RXDS;
    }
    if (!txn) {
        s->usr2 |= USR2_TXFE;
        s->uts |= UTS_TXEMPTY;
        if (!s->tx_busy && !s->tx_stalled) {
            s->usr2 |= USR2_TXDC;
        }
    }
    if (rxn) {
        s->usr2 |= USR2_RDR;
    } else {
        s->uts |= UTS_RXEMPTY;
    }
    if (txn == IMX8MM_UART_FIFO_SIZE) {
        s->uts |= UTS_TXFULL;
    }
    if (rxn == IMX8MM_UART_FIFO_SIZE) {
        s->uts |= UTS_RXFULL;
    }

    /* Interrupt sources and their enables */
    u1 = s->usr1;
    u2 = s->usr2;
    irq = ((u1 & USR1_TRDY) && (s->ucr1 & UCR1_TRDYEN)) ||
          ((u2 & USR2_IDLE) && (s->ucr1 & UCR1_IDEN)) ||
          ((u1 & USR1_RRDY) && (s->ucr1 & UCR1_RRDYEN)) ||
          ((u2 & USR2_TXFE) && (s->ucr1 & UCR1_TXMPTYEN)) ||
          ((u1 & USR1_RTSD) && (s->ucr1 & UCR1_RTSDEN)) ||
          ((u2 & USR2_ADET) && (s->ucr1 & UCR1_ADEN)) ||
          ((u1 & USR1_ESCF) && (s->ucr2 & UCR2_ESCI)) ||
          ((u2 & USR2_RTSF) && (s->ucr2 & UCR2_RTSEN)) ||
          ((u1 & USR1_AGTIM) && (s->ucr2 & UCR2_ATEN)) ||
          ((u2 & USR2_DTRF) && (s->ucr3 & UCR3_DTREN)) ||
          ((u1 & USR1_PARITYERR) && (s->ucr3 & UCR3_PARERREN)) ||
          ((u1 & USR1_FRAMERR) && (s->ucr3 & UCR3_FRAERREN)) ||
          ((u1 & USR1_RXDS) && (s->ucr3 & UCR3_RXDSEN)) ||
          ((u1 & USR1_AIRINT) && (s->ucr3 & UCR3_AIRINTEN)) ||
          ((u1 & USR1_AWAKE) && (s->ucr3 & UCR3_AWAKEN)) ||
          ((u1 & USR1_DTRD) && (s->ucr3 & UCR3_DTRDEN)) ||
          ((u2 & USR2_IRINT) && (s->ucr4 & UCR4_ENIRI)) ||
          ((u2 & USR2_WAKE) && (s->ucr4 & UCR4_WKEN)) ||
          ((u2 & USR2_TXDC) && (s->ucr4 & UCR4_TCEN)) ||
          ((u2 & USR2_BRCD) && (s->ucr4 & UCR4_BKEN)) ||
          ((u2 & USR2_ORE) && (s->ucr4 & UCR4_OREN)) ||
          ((u2 & USR2_RDR) && (s->ucr4 & UCR4_DREN));

    rx = ((s->ucr1 & UCR1_RXDMAEN) && (u1 & USR1_RRDY)) ||
         ((s->ucr1 & UCR1_ATDMAEN) && (u1 & USR1_AGTIM)) ||
         ((s->ucr4 & UCR4_IDDMAEN) && (u2 & USR2_IDLE));
    tx = (s->ucr1 & UCR1_TXDMAEN) && (u1 & USR1_TRDY);

    qemu_set_irq(s->irq, irq);
    qemu_set_irq(s->dma_rx, rx);
    qemu_set_irq(s->dma_tx, tx);
}

/*
 * Ageing timer: runs while the RX FIFO holds data below the RX threshold,
 * restarted by every received character and every URXD read.
 */
static void uart_ageing_restart(IMX8MMUARTState *s)
{
    uint32_t n = fifo32_num_used(&s->rx_fifo);

    if (n && n < (s->ufcr & UFCR_TL_MASK)) {
        timer_mod(&s->ageing_timer, uart_after_frames(s, AGEING_FRAMES));
    } else {
        timer_del(&s->ageing_timer);
    }
}

static void uart_ageing_expired(void *opaque)
{
    IMX8MMUARTState *s = opaque;

    s->usr1 |= USR1_AGTIM;
    uart_update(s);
}

static void uart_idle_expired(void *opaque)
{
    IMX8MMUARTState *s = opaque;

    s->usr2 |= USR2_IDLE;
    uart_update(s);
}

/* A complete character (data and URXD flags) enters the RX FIFO */
static void uart_rx_push(IMX8MMUARTState *s, uint32_t v)
{
    uint32_t icd = (s->ucr1 >> UCR1_ICD_SHIFT) & 3;

    if (fifo32_is_full(&s->rx_fifo)) {
        s->usr2 |= USR2_ORE;
    } else {
        /* RM: OVRRUN (and ERR) mark the character stored in position 32 */
        if (fifo32_num_used(&s->rx_fifo) == IMX8MM_UART_FIFO_SIZE - 1) {
            v |= URXD_ERR | URXD_OVRRUN;
        }
        fifo32_push(&s->rx_fifo, v);
    }
    if (v & URXD_BRK) {
        s->usr2 |= USR2_BRCD;
    }
    if (v & URXD_FRMERR) {
        s->usr1 |= USR1_FRAMERR;
    }
    uart_ageing_restart(s);
    /* Idle condition: more than 4 << ICD frames without a new start bit */
    timer_mod(&s->idle_timer, uart_after_frames(s, 4u << icd));
    uart_update(s);
}

/* Start bit of a character from the line */
static void uart_rx_start(IMX8MMUARTState *s, uint32_t v)
{
    s->usr1 |= USR1_AWAKE;
    s->usr2 |= USR2_WAKE;
    timer_del(&s->idle_timer);
    if (!s->line_timing) {
        uart_rx_push(s, v);
        return;
    }
    s->rx_busy = true;
    s->rx_char = v;
    timer_mod(&s->rx_timer, uart_after_frames(s, 1));
    uart_update(s);
}

static void uart_rx_done(void *opaque)
{
    IMX8MMUARTState *s = opaque;

    s->rx_busy = false;
    uart_rx_push(s, s->rx_char);
    qemu_chr_fe_accept_input(&s->chr);
}

static int uart_can_receive(void *opaque)
{
    IMX8MMUARTState *s = opaque;

    if (!uart_enabled(s, UCR2_RXEN) || (s->uts & UTS_LOOP)) {
        return 0;
    }
    if (s->line_timing) {
        return !s->rx_busy && !fifo32_is_full(&s->rx_fifo);
    }
    return fifo32_num_free(&s->rx_fifo);
}

static void uart_receive(void *opaque, const uint8_t *buf, int size)
{
    IMX8MMUARTState *s = opaque;

    for (int i = 0; i < size; i++) {
        uart_rx_start(s, buf[i]);
    }
}

static void uart_event(void *opaque, QEMUChrEvent event)
{
    IMX8MMUARTState *s = opaque;

    if (event == CHR_EVENT_BREAK && uart_enabled(s, UCR2_RXEN)) {
        uart_rx_start(s, URXD_BRK | URXD_FRMERR | URXD_ERR);
    }
}

static void uart_tx_start(IMX8MMUARTState *s);

static gboolean uart_tx_writable(void *do_not_use, GIOCondition cond,
                                 void *opaque);

/*
 * Put the character in the shift register on the line. Returns false if the
 * backend cannot take it now; the transmitter then waits for the backend.
 */
static bool uart_tx_emit(IMX8MMUARTState *s)
{
    uint8_t c = s->tx_char;

    if (s->uts & UTS_LOOP) {
        /* Internal loopback: the receiver sees the character instead */
        if (uart_enabled(s, UCR2_RXEN)) {
            uart_rx_push(s, c);
        }
        return true;
    }
    if (!qemu_chr_fe_backend_connected(&s->chr) ||
        qemu_chr_fe_write(&s->chr, &c, 1) == 1) {
        return true;
    }
    s->tx_watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                        uart_tx_writable, s);
    if (!s->tx_watch) {
        /* The backend cannot report when it is writable again: drop */
        return true;
    }
    s->tx_stalled = true;
    return false;
}

/* The character went out: the shifter is busy for one frame */
static void uart_tx_sent(IMX8MMUARTState *s)
{
    if (s->line_timing) {
        s->tx_busy = true;
        timer_mod(&s->tx_timer, uart_after_frames(s, 1));
    }
}

static gboolean uart_tx_writable(void *do_not_use, GIOCondition cond,
                                 void *opaque)
{
    IMX8MMUARTState *s = opaque;

    s->tx_watch = 0;
    s->tx_stalled = false;
    if (uart_tx_emit(s)) {
        uart_tx_sent(s);
        uart_tx_start(s);
    }
    return G_SOURCE_REMOVE;
}

static void uart_tx_done(void *opaque)
{
    IMX8MMUARTState *s = opaque;

    s->tx_busy = false;
    uart_tx_start(s);
}

/* Move characters from the TX FIFO into the shifter while it is free */
static void uart_tx_start(IMX8MMUARTState *s)
{
    while (!s->tx_busy && !s->tx_stalled && uart_enabled(s, UCR2_TXEN) &&
           !fifo32_is_empty(&s->tx_fifo)) {
        s->tx_char = fifo32_pop(&s->tx_fifo);
        if (!uart_tx_emit(s)) {
            break;
        }
        uart_tx_sent(s);
    }
    uart_update(s);
}

/* UCR2.SRST = 0: FIFOs, state machines, USR1/2, UBIR/UBMR, UTS status */
static void uart_soft_reset(IMX8MMUARTState *s)
{
    fifo32_reset(&s->rx_fifo);
    fifo32_reset(&s->tx_fifo);
    s->tx_busy = false;
    s->tx_stalled = false;
    if (s->tx_watch) {
        g_source_remove(s->tx_watch);
        s->tx_watch = 0;
    }
    s->rx_busy = false;
    timer_del(&s->tx_timer);
    timer_del(&s->rx_timer);
    timer_del(&s->ageing_timer);
    timer_del(&s->idle_timer);
    s->usr1 = USR1_RESET;
    s->usr2 = USR2_RESET;
    s->ubir = 0;
    s->ubmr = 0;
    s->uts &= UTS_RW_MASK;
    s->urxd_last = 0;
}

static uint64_t uart_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMUARTState *s = opaque;
    uint32_t v;

    switch (offset) {
    case URXD:
        if (fifo32_is_empty(&s->rx_fifo)) {
            /* RM: CHARRDY clear, the old data is invalid */
            return s->urxd_last & ~URXD_CHARRDY;
        }
        v = fifo32_pop(&s->rx_fifo) | URXD_CHARRDY;
        s->urxd_last = v;
        uart_ageing_restart(s);
        uart_update(s);
        qemu_chr_fe_accept_input(&s->chr);
        return v;
    case UTXD:
        return 0;
    case UCR1:
        return s->ucr1;
    case UCR2:
        return s->ucr2;
    case UCR3:
        return s->ucr3;
    case UCR4:
        return s->ucr4;
    case UFCR:
        return s->ufcr;
    case USR1:
        return s->usr1;
    case USR2:
        return s->usr2;
    case UESC:
        return s->uesc;
    case UTIM:
        return s->utim;
    case UBIR:
        return s->ubir;
    case UBMR:
        return s->ubmr;
    case UBRC:
        return UBRC_RESET;
    case ONEMS:
        return s->onems;
    case UTS:
        return s->uts;
    case UMCR:
        return s->umcr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read of reserved offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_UART, offset);
        return 0;
    }
}

static void uart_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    IMX8MMUARTState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case UTXD:
        if (fifo32_is_full(&s->tx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write to a full TX FIFO\n",
                          TYPE_IMX8MM_UART);
            break;
        }
        fifo32_push(&s->tx_fifo, v & 0xff);
        uart_tx_start(s);
        break;
    case UCR1:
        s->ucr1 = v & 0xffff;
        uart_tx_start(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case UCR2:
        if (!(v & UCR2_SRST)) {
            /* The reset completes at once, SRST reads back as 1 */
            uart_soft_reset(s);
            v |= UCR2_SRST;
        }
        s->ucr2 = v & 0xffff;
        uart_tx_start(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case UCR3:
        s->ucr3 = v & 0xffff;
        uart_update(s);
        break;
    case UCR4:
        s->ucr4 = v & 0xffff;
        uart_update(s);
        break;
    case UFCR:
        s->ufcr = v & 0xffff;
        uart_ageing_restart(s);
        uart_update(s);
        break;
    case USR1:
        s->usr1 &= ~(v & USR1_W1C);
        uart_update(s);
        break;
    case USR2:
        s->usr2 &= ~(v & USR2_W1C);
        uart_update(s);
        break;
    case UESC:
        s->uesc = v & 0xff;
        break;
    case UTIM:
        s->utim = v & 0xfff;
        break;
    case UBIR:
        s->ubir = v & 0xffff;
        break;
    case UBMR:
        s->ubmr = v & 0xffff;
        break;
    case ONEMS:
        s->onems = v & 0xffffff;
        break;
    case UTS:
        s->uts = (s->uts & ~UTS_RW_MASK) | (v & UTS_RW_MASK);
        uart_update(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case UMCR:
        s->umcr = v & 0xff0f;
        break;
    case URXD:
    case UBRC:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_UART, offset);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to reserved offset 0x%"
                      HWADDR_PRIx "\n", TYPE_IMX8MM_UART, offset);
        break;
    }
}

static const MemoryRegionOps uart_ops = {
    .read = uart_read,
    .write = uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void uart_reset(DeviceState *dev)
{
    IMX8MMUARTState *s = IMX8MM_UART(dev);

    s->uts = UTS_RESET;
    uart_soft_reset(s);
    s->ucr3 = UCR3_RESET;
    s->ucr4 = UCR4_RESET;
    s->ufcr = UFCR_RESET;
    s->uesc = UESC_RESET;
    s->utim = 0;
    s->onems = 0;
    s->umcr = 0;
    /*
     * The boot ROM leaves the UART enabled for transmission, so output of
     * a kernel loaded directly (and its early console) is visible.
     */
    s->ucr1 = UCR1_UARTEN;
    s->ucr2 = UCR2_SRST | UCR2_TXEN;
    uart_update(s);
}

static void uart_realize(DeviceState *dev, Error **errp)
{
    IMX8MMUARTState *s = IMX8MM_UART(dev);

    if (!s->ref_clock_hz) {
        error_setg(errp, "%s: ref-clock-hz must not be 0", TYPE_IMX8MM_UART);
        return;
    }
    fifo32_create(&s->rx_fifo, IMX8MM_UART_FIFO_SIZE);
    fifo32_create(&s->tx_fifo, IMX8MM_UART_FIFO_SIZE);
    timer_init_ns(&s->tx_timer, QEMU_CLOCK_VIRTUAL, uart_tx_done, s);
    timer_init_ns(&s->rx_timer, QEMU_CLOCK_VIRTUAL, uart_rx_done, s);
    timer_init_ns(&s->ageing_timer, QEMU_CLOCK_VIRTUAL, uart_ageing_expired,
                  s);
    timer_init_ns(&s->idle_timer, QEMU_CLOCK_VIRTUAL, uart_idle_expired, s);
    qemu_chr_fe_set_handlers(&s->chr, uart_can_receive, uart_receive,
                             uart_event, NULL, s, NULL, true);
}

static void uart_init(Object *obj)
{
    IMX8MMUARTState *s = IMX8MM_UART(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &uart_ops, s, TYPE_IMX8MM_UART,
                          IMX8MM_UART_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(dev, &s->irq, IMX8MM_UART_GPIO_IRQ, 1);
    qdev_init_gpio_out_named(dev, &s->dma_rx, IMX8MM_UART_GPIO_DMA_RX, 1);
    qdev_init_gpio_out_named(dev, &s->dma_tx, IMX8MM_UART_GPIO_DMA_TX, 1);
}

static int uart_post_load(void *opaque, int version_id)
{
    IMX8MMUARTState *s = opaque;

    /* A transmitter waiting for the backend waits again */
    if (s->tx_stalled && !s->tx_watch) {
        s->tx_watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                            uart_tx_writable, s);
        if (!s->tx_watch) {
            s->tx_stalled = false;
        }
    }
    return 0;
}

static const VMStateDescription vmstate_uart = {
    .name = TYPE_IMX8MM_UART,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = uart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ucr1, IMX8MMUARTState),
        VMSTATE_UINT32(ucr2, IMX8MMUARTState),
        VMSTATE_UINT32(ucr3, IMX8MMUARTState),
        VMSTATE_UINT32(ucr4, IMX8MMUARTState),
        VMSTATE_UINT32(ufcr, IMX8MMUARTState),
        VMSTATE_UINT32(usr1, IMX8MMUARTState),
        VMSTATE_UINT32(usr2, IMX8MMUARTState),
        VMSTATE_UINT32(uesc, IMX8MMUARTState),
        VMSTATE_UINT32(utim, IMX8MMUARTState),
        VMSTATE_UINT32(ubir, IMX8MMUARTState),
        VMSTATE_UINT32(ubmr, IMX8MMUARTState),
        VMSTATE_UINT32(onems, IMX8MMUARTState),
        VMSTATE_UINT32(uts, IMX8MMUARTState),
        VMSTATE_UINT32(umcr, IMX8MMUARTState),
        VMSTATE_UINT32(urxd_last, IMX8MMUARTState),
        VMSTATE_FIFO32(rx_fifo, IMX8MMUARTState),
        VMSTATE_FIFO32(tx_fifo, IMX8MMUARTState),
        VMSTATE_BOOL(tx_busy, IMX8MMUARTState),
        VMSTATE_BOOL(tx_stalled, IMX8MMUARTState),
        VMSTATE_UINT32(tx_char, IMX8MMUARTState),
        VMSTATE_BOOL(rx_busy, IMX8MMUARTState),
        VMSTATE_UINT32(rx_char, IMX8MMUARTState),
        VMSTATE_TIMER(tx_timer, IMX8MMUARTState),
        VMSTATE_TIMER(rx_timer, IMX8MMUARTState),
        VMSTATE_TIMER(ageing_timer, IMX8MMUARTState),
        VMSTATE_TIMER(idle_timer, IMX8MMUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property uart_properties[] = {
    DEFINE_PROP_CHR("chardev", IMX8MMUARTState, chr),
    /* UARTn_CLK_ROOT: CCM_TARGET_ROOT offset of the clock slice */
    DEFINE_PROP_LINK("ccm", IMX8MMUARTState, ccm, TYPE_IMX8MM_CCM,
                     IMX8MMCCMState *),
    DEFINE_PROP_UINT32("ccm-root", IMX8MMUARTState, ccm_root, 0),
    /* Reference clock without a CCM link (24 MHz oscillator) */
    DEFINE_PROP_UINT32("ref-clock-hz", IMX8MMUARTState, ref_clock_hz,
                       24000000),
    /* Shift characters at the programmed baud rate, or instantly */
    DEFINE_PROP_BOOL("line-timing", IMX8MMUARTState, line_timing, true),
};

static void uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = uart_realize;
    dc->vmsd = &vmstate_uart;
    dc->desc = "i.MX 8M Mini UART";
    device_class_set_legacy_reset(dc, uart_reset);
    device_class_set_props(dc, uart_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo imx8mm_uart_types[] = {
    {
        .name          = TYPE_IMX8MM_UART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX8MMUARTState),
        .instance_init = uart_init,
        .class_init    = uart_class_init,
    },
};

DEFINE_TYPES(imx8mm_uart_types)
