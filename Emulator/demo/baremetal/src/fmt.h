/*
 * Minimal formatted output: %c %s %d %u %x %X %p with optional '0' flag,
 * width and the 'l'/'ll' length modifiers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_FMT_H
#define KICK_DEMO_FMT_H

#include <stdarg.h>
#include <stddef.h>

typedef void (*fmt_out_fn)(char c, void *ctx);

int fmt_vformat(fmt_out_fn out, void *ctx, const char *f, va_list ap);
int fmt_snprintf(char *buf, size_t size, const char *f, ...)
    __attribute__((format(printf, 3, 4)));

void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);

#endif
