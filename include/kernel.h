#ifndef INCLUDE_KERNEL_H
#define INCLUDE_KERNEL_H

#include "../lib/types.h"

/* Global kernel identity. */
#define KERNEL_NAME      "Trinux"
#define KERNEL_VERSION   "0.2.0"
#define KERNEL_ARCH      "i686"
#define KERNEL_BUILD     "x86 32-bit protected mode"
#define DEFAULT_USER     "user"

/* Handy macros. */
#define UNUSED(x)        ((void)(x))
#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* kernel.c */
void kernel_main(uint32_t magic, uint32_t mb_info_addr);

/* panic.c */
void panic(const char *msg);

#endif /* INCLUDE_KERNEL_H */
