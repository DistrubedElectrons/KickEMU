/*
 * i.MX UART driver (polled TX, interrupt driven RX)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_UART_H
#define KICK_DEMO_UART_H

#include <stdint.h>

typedef void (*uart_rx_fn)(uint8_t byte);

/* Program the UART for 8N1 at @baud from a @ref_hz module clock */
void uart_init(uint32_t base, uint32_t ref_hz, uint32_t baud);
void uart_putc(uint32_t base, uint8_t c);
void uart_write(uint32_t base, const uint8_t *buf, int len);
/* Enable the receive-ready interrupt and deliver bytes to @fn (IRQ context) */
void uart_enable_rx_irq(uint32_t base, uart_rx_fn fn);
/* Drain the RX FIFO into the registered callback; call from the IRQ */
void uart_rx_irq(uint32_t base);

#endif
