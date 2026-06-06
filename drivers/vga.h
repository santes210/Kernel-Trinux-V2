#ifndef DRIVERS_VGA_H
#define DRIVERS_VGA_H

#include "../lib/types.h"

/* The 16 VGA colors. */
enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14,
    VGA_WHITE = 15,
};

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
    return (uint8_t)fg | (uint8_t)(bg << 4);
}

void vga_init(void);
void vga_clear(void);
void vga_putchar(char c);
void vga_print(const char *str);
void vga_print_color(const char *str, uint8_t color);
void vga_scroll(void);
void vga_set_cursor(int x, int y);
void vga_set_color(uint8_t color);
uint8_t vga_get_color(void);
void vga_get_cursor(int *x, int *y);
void vga_printf(const char *fmt, ...);

/* Output capture: while a capture buffer is set, vga_putchar() writes there
 * instead of to the screen. Used to implement shell pipes (cmd1 | cmd2). */
void vga_capture_begin(char *buf, uint32_t cap);
uint32_t vga_capture_end(void);   /* returns number of bytes captured */
int      vga_capture_active(void);

#endif /* DRIVERS_VGA_H */
