/* drivers/serial.c  -  COM1 serial port for debug output. */
#include "serial.h"
#include "../cpu/ports.h"
#include "../lib/printf.h"

void serial_init(void)
{
    outb(COM1 + 1, 0x00);    /* disable interrupts */
    outb(COM1 + 3, 0x80);    /* enable DLAB (baud divisor) */
    outb(COM1 + 0, 0x03);    /* divisor low: 38400 baud */
    outb(COM1 + 1, 0x00);    /* divisor high */
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7);    /* enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int serial_tx_empty(void)
{
    return inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c)
{
    if (c == '\n')
        serial_write_char('\r');
    while (!serial_tx_empty())
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *str)
{
    while (*str)
        serial_write_char(*str++);
}

void serial_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_write(buf);
}
