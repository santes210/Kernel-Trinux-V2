#ifndef LIB_PRINTF_H
#define LIB_PRINTF_H

#include "types.h"

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list args);
int  snprintf(char *buf, size_t size, const char *fmt, ...);
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

#endif /* LIB_PRINTF_H */
