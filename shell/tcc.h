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
 *   - Variables: local, arrays (int arr[N])
 *   - Functions: definition and calls (including forward references)
 *   - Control: if/else, while, do-while, for, return, break, continue
 *   - Operators: + - * / % == != < > <= >= && || ! & | ^
 *   - Assignment: = += -= *= /= %=
 *   - Increment/Decrement: ++ --  (prefix and postfix)
 *   - Address-of: &var
 *   - Dereference: *ptr
 *   - Strings: "hello\n"
 *   - Arrays: int arr[10]; arr[i] = x; x = arr[i];
 *   - Multiple declarations: int x, y, z;
 *   - Comments: // single-line  and  multi-line
 *
 * Built-in functions (linked automatically):
 *   print(str)              print a string
 *   print_num(n)            print an integer (handles negatives)
 *   print_char(c)           print a single character
 *   getchar()               read a keypress (blocking)
 *   getline(buf, max)       read a line with echo + backspace support
 *   sleep(ms)               sleep for ms milliseconds
 *   uptime()                seconds since boot
 *   getpid()                process ID
 *   exit(code)              exit the program
 *
 *   strlen(str)             length of a null-terminated string
 *   strcmp(a, b)            compare two strings (returns <0, 0, >0)
 *   strncmp(a, b, n)       compare at most n characters
 *   strcpy(dst, src)        copy string (returns dst)
 *   itoa(n, buf)            convert integer to string (returns buf)
 *
 *   vga_clear(color)        clear screen with color
 *   vga_putchar(x,y,ch,c)   put char at position
 *   vga_print(x,y,str,c)    print string at position
 */

int tcc_compile(const char *src_path, const char *out_path);

#endif /* SHELL_TCC_H */
