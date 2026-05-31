#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include "../lib/types.h"

#define COM1 0x3F8

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *str);
void serial_printf(const char *fmt, ...);

#endif /* DRIVERS_SERIAL_H */
