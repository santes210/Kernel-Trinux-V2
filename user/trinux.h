/* user/trinux.h  -  Trinux userspace library (mini libc for ring 3 programs).
 *
 * Programs running in ring 3 CANNOT use I/O ports (inb/outb), but CAN
 * access the VGA text buffer at 0xB8000 directly (it's in the identity-
 * mapped region with PAGE_USER).  This means you can build full-screen
 * text-mode apps, games, menus, etc.
 *
 * Compile your program with:
 *   gcc -m32 -ffreestanding -nostdlib -static \
 *     -Wl,-Ttext=0x08048000 -Wl,--entry=_start -o myapp myapp.c
 *
 * Then run in Trinux:
 *   exec /bin/myapp    or    /bin/myapp
 */
#ifndef USER_TRINUX_H
#define USER_TRINUX_H

typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef int            int32_t;

/* ---- Syscall numbers (must match cpu/syscall.h) ---- */
#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_GETPID  2
#define SYS_YIELD   3
#define SYS_SLEEP   4
#define SYS_GETC    5
#define SYS_UPTIME  6

/* ---- Syscall wrappers ---- */

static inline int _syscall0(int num) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int _syscall1(int num, int a) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a) : "memory");
    return ret;
}

static inline int _syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

/* ---- Basic functions ---- */

static inline void exit(int status) {
    _syscall1(SYS_EXIT, status);
    for(;;);
}

static inline void print(const char *s) {
    int len = 0;
    while (s[len]) len++;
    _syscall3(SYS_WRITE, 1, (int)s, len);
}

static inline void print_num(uint32_t n) {
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if (n == 0) { buf[i--] = '0'; }
    while (n && i >= 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    print(&buf[i + 1]);
}

static inline int getpid(void) { return _syscall0(SYS_GETPID); }
static inline void msleep(int ms) { _syscall1(SYS_SLEEP, ms); }
static inline int getchar(void) { return _syscall0(SYS_GETC); }
static inline uint32_t uptime(void) { return (uint32_t)_syscall0(SYS_UPTIME); }

/* ---- String helpers ---- */

static inline int strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static inline void *memset(void *dst, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

static inline void *memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* ============================================================================
 *  VGA TEXT MODE — Direct framebuffer access from userspace
 *
 *  The VGA text buffer lives at physical address 0xB8000 and is identity-
 *  mapped with PAGE_USER, so ring-3 programs can read/write it directly.
 *
 *  Screen: 80 columns × 25 rows = 2000 cells
 *  Each cell: 2 bytes = [character][attribute]
 *  Attribute byte: low 4 bits = foreground, high 4 bits = background
 * ============================================================================ */

#define VGA_BUF     ((uint16_t *)0xB8000)
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

/* Colors (same values as the kernel VGA driver) */
#define BLACK         0x0
#define BLUE          0x1
#define GREEN         0x2
#define CYAN          0x3
#define RED           0x4
#define MAGENTA       0x5
#define BROWN         0x6
#define LIGHT_GREY    0x7
#define DARK_GREY     0x8
#define LIGHT_BLUE    0x9
#define LIGHT_GREEN   0xA
#define LIGHT_CYAN    0xB
#define LIGHT_RED     0xC
#define LIGHT_MAGENTA 0xD
#define YELLOW        0xE
#define WHITE         0xF

/* Make a VGA color attribute byte. */
static inline uint8_t vga_color(uint8_t fg, uint8_t bg) {
    return (bg << 4) | (fg & 0x0F);
}

/* Make a VGA character+attribute word. */
static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

/* Clear the entire screen with a color. */
static inline void vga_clear(uint8_t color) {
    uint16_t blank = vga_entry(' ', color);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_BUF[i] = blank;
}

/* Put a character at (x, y) with a color. */
static inline void vga_putchar_at(int x, int y, char c, uint8_t color) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT)
        VGA_BUF[y * VGA_WIDTH + x] = vga_entry(c, color);
}

/* Read the character at (x, y). */
static inline char vga_getchar_at(int x, int y) {
    return (char)(VGA_BUF[y * VGA_WIDTH + x] & 0xFF);
}

/* Print a string at (x, y) with a color. Returns the number of chars printed. */
static inline int vga_print_at(int x, int y, const char *s, uint8_t color) {
    int n = 0;
    while (*s && x < VGA_WIDTH) {
        vga_putchar_at(x++, y, *s++, color);
        n++;
    }
    return n;
}

/* Print a number at (x, y). */
static inline void vga_print_num_at(int x, int y, uint32_t n, uint8_t color) {
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if (n == 0) { buf[i--] = '0'; }
    while (n && i >= 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    vga_print_at(x, y, &buf[i + 1], color);
}

/* Draw a horizontal line of a character. */
static inline void vga_hline(int x, int y, int len, char c, uint8_t color) {
    for (int i = 0; i < len && x + i < VGA_WIDTH; i++)
        vga_putchar_at(x + i, y, c, color);
}

/* Draw a vertical line of a character. */
static inline void vga_vline(int x, int y, int len, char c, uint8_t color) {
    for (int i = 0; i < len && y + i < VGA_HEIGHT; i++)
        vga_putchar_at(x, y + i, c, color);
}

/* Fill a rectangle. */
static inline void vga_fill_rect(int x, int y, int w, int h,
                                 char c, uint8_t color) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            vga_putchar_at(x + dx, y + dy, c, color);
}

/* Draw a box with single-line border characters. */
static inline void vga_box(int x, int y, int w, int h, uint8_t color) {
    /* corners */
    vga_putchar_at(x, y, '+', color);
    vga_putchar_at(x + w - 1, y, '+', color);
    vga_putchar_at(x, y + h - 1, '+', color);
    vga_putchar_at(x + w - 1, y + h - 1, '+', color);
    /* top/bottom */
    vga_hline(x + 1, y, w - 2, '-', color);
    vga_hline(x + 1, y + h - 1, w - 2, '-', color);
    /* left/right */
    vga_vline(x, y + 1, h - 2, '|', color);
    vga_vline(x + w - 1, y + 1, h - 2, '|', color);
}

/* Simple "non-blocking" key check: returns -1 if no key, or the key.
 * NOTE: getchar() blocks; this just checks if a key is pending.
 * Since we don't have a trygetchar syscall, we use getchar (blocking).
 * For games you'll want to use getchar() in a loop with msleep(). */

/* ---- Entry point ---- */
extern int main(void);

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    int ret = main();
    exit(ret);
}

#endif /* USER_TRINUX_H */

