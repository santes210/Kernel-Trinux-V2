#ifndef SHELL_TCC_H
#define SHELL_TCC_H

/* tcc — Trinux C Compiler.
 *
 * A minimal C compiler built into the kernel shell that compiles a subset
 * of C into ELF32 executables.  Programs can use built-in functions for
 * screen output, input, VGA access, and system calls.
 *
 * Usage:
 *   edit myapp.c           (write your C code)
 *   tcc myapp.c            (compile → creates 'myapp' ELF)
 *   exec myapp             (run it)
 *
 * Supported C subset:
 *   - Types: int, char (both 32-bit internally)
 *   - Variables: local and global
 *   - Functions: definition and calls
 *   - Control: if/else, while, for, return
 *   - Operators: + - * / % == != < > <= >= && || ! & | ^
 *   - Strings: "hello\n"
 *   - Arrays: int arr[10]; arr[i] = x;
 *   - Pointers: basic *(addr) syntax
 *
 * Built-in functions (linked automatically):
 *   print(str)             — print a string
 *   print_num(n)           — print a number
 *   print_char(c)          — print a single character
 *   getchar()              — read a keypress (blocking)
 *   sleep(ms)              — sleep for ms milliseconds
 *   uptime()               — seconds since boot
 *   getpid()               — process ID
 *   exit(code)             — exit the program
 *   vga_clear(color)       — clear screen
 *   vga_putchar(x,y,ch,c)  — put char at position
 *   vga_print(x,y,str,c)   — print string at position
 */

int tcc_compile(const char *src_path, const char *out_path);

#endif /* SHELL_TCC_H */
