/*
 * Minimal formatted output and the few libc helpers the compiler may call.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdint.h>

#include "fmt.h"

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = s;

    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;

    while (n--) {
        *dp++ = *sp++;
    }
    return d;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static int fmt_number(fmt_out_fn out, void *ctx, unsigned long long v,
                      int base, int upper, int width, char pad, int neg)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0, count = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg && pad == '0') {
        out('-', ctx);
        count++;
    } else if (neg) {
        tmp[n++] = '-';
    }
    while (count + n < width) {
        out(pad, ctx);
        count++;
    }
    while (n) {
        out(tmp[--n], ctx);
        count++;
    }
    return count;
}

int fmt_vformat(fmt_out_fn out, void *ctx, const char *f, va_list ap)
{
    int count = 0;

    for (; *f; f++) {
        char pad = ' ';
        int width = 0, lng = 0;
        unsigned long long u;
        long long s;
        const char *str;

        if (*f != '%') {
            out(*f, ctx);
            count++;
            continue;
        }
        f++;
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f++ - '0');
        }
        while (*f == 'l') {
            lng++;
            f++;
        }
        switch (*f) {
        case 'c':
            out((char)va_arg(ap, int), ctx);
            count++;
            break;
        case 's':
            str = va_arg(ap, const char *);
            for (int n = (int)strlen(str); n < width; n++) {
                out(' ', ctx);
                count++;
            }
            while (*str) {
                out(*str++, ctx);
                count++;
            }
            break;
        case 'd':
            s = lng ? va_arg(ap, long long) : va_arg(ap, int);
            count += fmt_number(out, ctx, s < 0 ? -(unsigned long long)s : s,
                                10, 0, width, pad, s < 0);
            break;
        case 'u':
        case 'x':
        case 'X':
            u = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            count += fmt_number(out, ctx, u, *f == 'u' ? 10 : 16, *f == 'X',
                                width, pad, 0);
            break;
        case 'p':
            out('0', ctx);
            out('x', ctx);
            count += 2 + fmt_number(out, ctx, (uintptr_t)va_arg(ap, void *),
                                    16, 0, 0, '0', 0);
            break;
        case '%':
            out('%', ctx);
            count++;
            break;
        default:
            break;
        }
    }
    return count;
}

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} fmt_buf;

static void fmt_buf_out(char c, void *ctx)
{
    fmt_buf *b = ctx;

    if (b->pos + 1 < b->size) {
        b->buf[b->pos] = c;
    }
    b->pos++;
}

int fmt_snprintf(char *buf, size_t size, const char *f, ...)
{
    fmt_buf b = { buf, size, 0 };
    va_list ap;
    int n;

    va_start(ap, f);
    n = fmt_vformat(fmt_buf_out, &b, f, ap);
    va_end(ap);
    if (size) {
        buf[b.pos < size ? b.pos : size - 1] = 0;
    }
    return n;
}
