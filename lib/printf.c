/* lib/printf.c  -  formatted output for the kernel.
 *
 * Supports: %d %i %u %x %X %p %s %c %%
 * Flags/width: zero-pad and left-justify with width, e.g. %08x, %-20s, %5d.
 *
 * Output is routed through a sink so both VGA console output (kprintf) and
 * buffer output (snprintf) share the same engine.
 */
#include "printf.h"
#include "string.h"
#include "../drivers/vga.h"
#include "../drivers/serial.h"

struct sink {
    char  *buf;       /* NULL => write to VGA */
    size_t size;      /* capacity for buffer mode */
    size_t count;     /* total chars that would have been written */
};

static void sink_putc(struct sink *s, char c)
{
    if (s->buf) {
        if (s->count + 1 < s->size)
            s->buf[s->count] = c;
    } else {
        vga_putchar(c);
        serial_write_char(c);
    }
    s->count++;
}

static void sink_puts(struct sink *s, const char *str)
{
    while (*str)
        sink_putc(s, *str++);
}

/* Emit a string padded to `width` with `pad`, optionally left-justified. */
static void emit_padded(struct sink *s, const char *str, int width,
                        bool left, char pad)
{
    int len = (int)strlen(str);
    int fill = width - len;
    if (fill < 0)
        fill = 0;

    if (!left)
        for (int i = 0; i < fill; i++)
            sink_putc(s, pad);

    sink_puts(s, str);

    if (left)
        for (int i = 0; i < fill; i++)
            sink_putc(s, ' ');
}

static void format(struct sink *s, const char *fmt, va_list args)
{
    char numbuf[36];

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_putc(s, *fmt);
            continue;
        }

        fmt++; /* skip '%' */

        bool left = false;
        char pad = ' ';
        int  width = 0;

        /* flags */
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            fmt++;
        }
        /* width */
        while (isdigit((unsigned char)*fmt)) {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = va_arg(args, int);
            itoa(v, numbuf, 10);
            emit_padded(s, numbuf, width, left, pad);
            break;
        }
        case 'u': {
            unsigned int v = va_arg(args, unsigned int);
            /* unsigned decimal */
            char tmp[12];
            int i = 0;
            if (v == 0) tmp[i++] = '0';
            while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
            int j = 0;
            while (i > 0) numbuf[j++] = tmp[--i];
            numbuf[j] = '\0';
            emit_padded(s, numbuf, width, left, pad);
            break;
        }
        case 'x': {
            uint32_t v = va_arg(args, uint32_t);
            itoa_hex(v, numbuf, false);
            emit_padded(s, numbuf, width, left, pad);
            break;
        }
        case 'X': {
            uint32_t v = va_arg(args, uint32_t);
            itoa_hex(v, numbuf, true);
            emit_padded(s, numbuf, width, left, pad);
            break;
        }
        case 'p': {
            uint32_t v = (uint32_t)va_arg(args, void *);
            char hex[12];
            itoa_hex(v, hex, false);
            numbuf[0] = '0';
            numbuf[1] = 'x';
            strcpy(numbuf + 2, hex);
            emit_padded(s, numbuf, width, left, pad);
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            char one[2] = { c, '\0' };
            emit_padded(s, one, width, left, ' ');
            break;
        }
        case 's': {
            const char *str = va_arg(args, const char *);
            if (!str) str = "(null)";
            emit_padded(s, str, width, left, ' ');
            break;
        }
        case '%':
            sink_putc(s, '%');
            break;
        case '\0':
            return;
        default:
            sink_putc(s, '%');
            sink_putc(s, *fmt);
            break;
        }
    }
}

void kvprintf(const char *fmt, va_list args)
{
    struct sink s = { NULL, 0, 0 };
    format(&s, fmt, args);
}

void kprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    struct sink s = { buf, size, 0 };
    format(&s, fmt, args);
    if (size > 0) {
        size_t term = (s.count < size) ? s.count : size - 1;
        buf[term] = '\0';
    }
    return (int)s.count;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}
