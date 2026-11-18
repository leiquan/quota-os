#include "quotaos.h"
#include <stdarg.h>

static void put_s(const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        uart_putc(*s++);
}

static void put_u(uint64_t v, int base, int width, char pad)
{
    char tmp[32];
    int i = 0;
    if (base < 2)
        base = 10;
    if (v == 0)
        tmp[i++] = '0';
    while (v) {
        uint64_t d = v % (uint64_t)base;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= (uint64_t)base;
    }
    while (i < width)
        tmp[i++] = pad;
    while (i--)
        uart_putc(tmp[i]);
}

void printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            uart_putc(*fmt++);
            continue;
        }
        fmt++;
        char pad = ' ';
        int left = 0;
        int width = 0;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int lng = 0;
        if (*fmt == 'l') {
            lng = 1;
            fmt++;
            if (*fmt == 'l')
                fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            if (!left)
                while (width > len) {
                    uart_putc(pad);
                    width--;
                }
            put_s(s);
            if (left)
                while (width > len) {
                    uart_putc(' ');
                    width--;
                }
            break;
        }
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                uart_putc('-');
                put_u((uint64_t)(-v), 10, width, pad);
            } else {
                put_u((uint64_t)v, 10, width, pad);
            }
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_u(v, 10, width, pad);
            break;
        }
        case 'x': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_u(v, 16, width, pad);
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case '%':
            uart_putc('%');
            break;
        case 0:
            va_end(ap);
            return;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
        fmt++;
    }
    va_end(ap);
}

/* 按显示宽度补空格：CJK 和全角字符算 2 列，其余算 1 列。 */
void put_padded(const char *s, int cols)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int n;
        if (*p < 0x80) {
            cp = *p;
            n = 1;
        } else if ((*p & 0xe0) == 0xc0) {
            cp = *p & 0x1f;
            n = 2;
        } else if ((*p & 0xf0) == 0xe0) {
            cp = *p & 0x0f;
            n = 3;
        } else {
            cp = *p & 0x07;
            n = 4;
        }
        for (int i = 1; i < n && p[i]; i++)
            cp = (cp << 6) | (p[i] & 0x3f);
        w += (cp >= 0x1100 && (cp <= 0x115f || (cp >= 0x2e80 && cp <= 0xa4cf) ||
                               (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
                               (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
                               (cp >= 0xffe0 && cp <= 0xffe6))) ? 2 : 1;
        for (int i = 0; i < n && *p; i++)
            uart_putc((char)*p++);
    }
    while (w++ < cols)
        uart_putc(' ');
}
