/*
 * Debug console on UART4.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdarg.h>

#include "console.h"
#include "fmt.h"
#include "imx8mm.h"
#include "uart.h"

void console_init(uint32_t ref_hz)
{
    uart_init(UART4_BASE, ref_hz, KICK_CONSOLE_BAUD);
}

static void console_out(char c, void *ctx)
{
    (void)ctx;
    if (c == '\n') {
        uart_putc(UART4_BASE, '\r');
    }
    uart_putc(UART4_BASE, (uint8_t)c);
}

void kprintf(const char *f, ...)
{
    va_list ap;

    va_start(ap, f);
    fmt_vformat(console_out, 0, f, ap);
    va_end(ap);
}
