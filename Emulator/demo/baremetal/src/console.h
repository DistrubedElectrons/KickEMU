/*
 * Debug console on UART4 (ttymxc3 in Linux)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_CONSOLE_H
#define KICK_DEMO_CONSOLE_H

#include <stdint.h>

void console_init(uint32_t ref_hz);
void kprintf(const char *f, ...) __attribute__((format(printf, 1, 2)));

/* ANSI colours for the banner */
#define C_RESET     "\033[0m"
#define C_BOLD      "\033[1m"
#define C_DIM       "\033[2m"
#define C_RED       "\033[31m"
#define C_GREEN     "\033[32m"
#define C_YELLOW    "\033[33m"
#define C_CYAN      "\033[36m"
#define C_MAGENTA   "\033[35m"

#endif
