#ifndef LIB_TYPES_H
#define LIB_TYPES_H

/* Freestanding fixed-width integer types (no libc available). */

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

typedef uint32_t           size_t;
typedef int32_t            ssize_t;
typedef uint32_t           uintptr_t;

typedef enum { false = 0, true = 1 } bool;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Variadic args (freestanding builtins from GCC). */
typedef __builtin_va_list  va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)

#endif /* LIB_TYPES_H */
