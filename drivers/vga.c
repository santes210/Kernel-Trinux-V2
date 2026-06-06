/* drivers/vga.c  -  VGA text-mode driver (0xB8000). */
#include "vga.h"
#include "../cpu/ports.h"
#include "../lib/printf.h"

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define TAB_WIDTH  4

static int     cursor_x;
static int     cursor_y;
static uint8_t cur_color;

/* ---- output capture (for shell pipes) ---- */
static char    *capture_buf;
static uint32_t capture_cap;
static uint32_t capture_len;

void vga_capture_begin(char *buf, uint32_t cap)
{
    capture_buf = buf;
    capture_cap = cap;
    capture_len = 0;
}

int vga_capture_active(void) { return capture_buf != NULL; }

uint32_t vga_capture_end(void)
{
    uint32_t n = capture_len;
    if (capture_buf && capture_len < capture_cap)
        capture_buf[capture_len] = '\0';
    capture_buf = NULL;
    capture_cap = 0;
    capture_len = 0;
    return n;
}

static inline uint16_t make_entry(char c, uint8_t color)
{
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void update_hw_cursor(void)
{
    uint16_t pos = (uint16_t)(cursor_y * VGA_WIDTH + cursor_x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_cursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    cursor_x = x;
    cursor_y = y;
    update_hw_cursor();
}

void vga_get_cursor(int *x, int *y)
{
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
}

void vga_set_color(uint8_t color) { cur_color = color; }
uint8_t vga_get_color(void)       { return cur_color; }

void vga_clear(void)
{
    uint16_t blank = make_entry(' ', cur_color);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEMORY[i] = blank;
    cursor_x = 0;
    cursor_y = 0;
    update_hw_cursor();
}

void vga_init(void)
{
    cur_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void vga_scroll(void)
{
    if (cursor_y < VGA_HEIGHT)
        return;

    /* move every line up by one */
    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[(y - 1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
    }
    /* clear last line */
    uint16_t blank = make_entry(' ', cur_color);
    for (int x = 0; x < VGA_WIDTH; x++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = blank;

    cursor_y = VGA_HEIGHT - 1;
}

void vga_putchar(char c)
{
    /* If output capture is active, redirect everything there (for pipes). */
    if (capture_buf) {
        if (capture_len + 1 < capture_cap)
            capture_buf[capture_len++] = c;
        return;
    }

    switch (c) {
    case '\n':
        cursor_x = 0;
        cursor_y++;
        break;
    case '\r':
        cursor_x = 0;
        break;
    case '\t':
        cursor_x = (cursor_x + TAB_WIDTH) & ~(TAB_WIDTH - 1);
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
        break;
    case '\b':
        if (cursor_x > 0) {
            cursor_x--;
        } else if (cursor_y > 0) {
            cursor_y--;
            cursor_x = VGA_WIDTH - 1;
        }
        VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = make_entry(' ', cur_color);
        break;
    default:
        VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = make_entry(c, cur_color);
        cursor_x++;
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
        break;
    }

    vga_scroll();
    update_hw_cursor();
}

void vga_print(const char *str)
{
    while (*str)
        vga_putchar(*str++);
}

void vga_print_color(const char *str, uint8_t color)
{
    uint8_t saved = cur_color;
    cur_color = color;
    while (*str)
        vga_putchar(*str++);
    cur_color = saved;
}

void vga_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
