#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include "../lib/types.h"

#define COM1 0x3F8

/* Bring up COM1 in 115200-8N1 with FIFO + RX interrupts on IRQ4 so that
 * anything pasted into `qemu ... -serial mon:stdio` (or any other COM1
 * connection) flows straight into the same input queue the PS/2 keyboard
 * uses.  Works perfectly with `-display none -serial mon:stdio` and is
 * the recommended path for Android / Termux clipboards: no scancodes are
 * involved, so QEMU never drops bytes and modifiers can't get stuck. */
void serial_init(void);          /* call early: configures UART, TX only */
void serial_enable_input(void);  /* call AFTER irq_install(): hooks IRQ4 + RX */

void serial_write_char(char c);
void serial_write(const char *str);
void serial_printf(const char *fmt, ...);

#endif /* DRIVERS_SERIAL_H */
