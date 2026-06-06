/* drivers/fb.h — Framebuffer driver (modo grafico VBE/VESA).
 *
 * API simetrica a drivers/vga.h pero pintando pixel-por-pixel en un
 * framebuffer lineal de 32 bits (RGB888 + padding). El kernel cae aqui
 * cuando GRUB nos pone en modo grafico via multiboot framebuffer info.
 */
#ifndef DRIVERS_FB_H
#define DRIVERS_FB_H

#include "../lib/types.h"

/* Init: detecta framebuffer del multiboot info. Devuelve true si arrancamos
 * en modo grafico, false si hay que fallback a VGA texto. */
bool fb_init(uint32_t mb_magic, uint32_t mb_info_addr);

/* True si el FB se inicializo correctamente (modo grafico activo). */
bool fb_is_active(void);

/* Dimensiones en cells de texto (depende de la resolucion y de la fuente). */
int fb_text_cols(void);
int fb_text_rows(void);

/* Dimensiones en pixels (utiles para programas graficos futuros). */
int fb_pixel_width(void);
int fb_pixel_height(void);

/* API de "modo texto sobre framebuffer" — espejo de vga.h. */
void fb_clear(void);
void fb_putchar(char c);
void fb_set_cursor(int col, int row);    /* posicion en cells */
void fb_get_cursor(int *col, int *row);
void fb_set_color(uint8_t color);        /* mismo formato VGA: bg<<4 | fg */
void fb_scroll(void);

/* Pixel access (para futuras apps graficas via syscalls). */
void fb_put_pixel(int x, int y, uint32_t rgb);
uint32_t fb_get_pixel(int x, int y);

#endif /* DRIVERS_FB_H */
