/*
 * i.MX UART driver (IMX8MMRM chapter 14.2 "UART").
 *
 * Baud rate: BAUD = ref_clk / (16 * (UBMR + 1) / (UBIR + 1)), with
 * ref_clk = module clock / RFDIV. We use RFDIV = 1 and UBIR + 1 = 100, so
 * UBMR + 1 = ref_clk * 100 / (16 * BAUD).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "imx8mm.h"
#include "uart.h"

#define URXD        0x00
#define UTXD        0x40
#define UCR1        0x80
#define UCR2        0x84
#define UCR3        0x88
#define UCR4        0x8c
#define UFCR        0x90
#define USR2        0x98
#define UBIR        0xa4
#define UBMR        0xa8
#define UTS         0xb4

#define UCR1_UARTEN     (1u << 0)
#define UCR1_RRDYEN     (1u << 9)
#define UCR2_SRST       (1u << 0)   /* active low */
#define UCR2_RXEN       (1u << 1)
#define UCR2_TXEN       (1u << 2)
#define UCR2_WS         (1u << 5)   /* 8 data bits */
#define UCR2_IRTS       (1u << 14)  /* ignore RTS */
#define UCR3_RXDMUXSEL  (1u << 2)   /* required on i.MX6 and later */
#define UFCR_RFDIV_1    (5u << 7)
#define UFCR_TXTL(n)    ((uint32_t)(n) << 10)
#define UFCR_RXTL(n)    ((uint32_t)(n) << 0)
#define USR2_RDR        (1u << 0)
#define UTS_TXFULL      (1u << 4)
#define URXD_DATA       0xff

/* RX callbacks for UART1 and UART4 (the two the demo uses) */
static uart_rx_fn rx_cb[2];

static int uart_slot(uint32_t base)
{
    return base == UART1_BASE ? 0 : 1;
}

void uart_init(uint32_t base, uint32_t ref_hz, uint32_t baud)
{
    uint32_t ubmr = (uint32_t)(((uint64_t)ref_hz * 100) / (16ull * baud)) - 1;

    wr32(base + UCR2, 0);               /* software reset */
    while (!(rd32(base + UCR2) & UCR2_SRST)) {
        /* SRST reads back 1 when the reset is done */
        wr32(base + UCR2, UCR2_SRST);
    }
    wr32(base + UCR1, UCR1_UARTEN);
    wr32(base + UCR3, UCR3_RXDMUXSEL);
    wr32(base + UCR4, 0);
    wr32(base + UFCR, UFCR_RFDIV_1 | UFCR_TXTL(2) | UFCR_RXTL(1));
    wr32(base + UBIR, 99);
    wr32(base + UBMR, ubmr);
    wr32(base + UCR2, UCR2_SRST | UCR2_RXEN | UCR2_TXEN | UCR2_WS |
                      UCR2_IRTS);
}

void uart_putc(uint32_t base, uint8_t c)
{
    while (rd32(base + UTS) & UTS_TXFULL) {
    }
    wr32(base + UTXD, c);
}

void uart_write(uint32_t base, const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        uart_putc(base, buf[i]);
    }
}

void uart_enable_rx_irq(uint32_t base, uart_rx_fn fn)
{
    rx_cb[uart_slot(base)] = fn;
    wr32(base + UCR1, rd32(base + UCR1) | UCR1_RRDYEN);
}

void uart_rx_irq(uint32_t base)
{
    uart_rx_fn fn = rx_cb[uart_slot(base)];

    while (rd32(base + USR2) & USR2_RDR) {
        uint8_t c = rd32(base + URXD) & URXD_DATA;

        if (fn) {
            fn(c);
        }
    }
}
