/* drivers/fb.c — Framebuffer driver (VBE/VESA, modo grafico).
 *
 * Renderiza texto pixel-por-pixel sobre un framebuffer lineal que GRUB
 * configura en respuesta a la peticion FB_VIDMODE del multiboot header.
 *
 * Modelo:
 *   - Recibe addr/pitch/width/height/bpp del multiboot info en fb_init().
 *   - Usa la fuente VGA 8x8 (declarada extern, vive en drivers/vga.c).
 *   - Mantiene "cell cursor" (col, row) similar al VGA text mode.
 *   - Cells de 8 pixels x 8 pixels (mismo aspect ratio que VGA text 8x8).
 *   - Soporta bpp = 32 (RGBA, mas comun en QEMU/VBE moderno).
 *     Otros bpp (8, 16, 24) caen al modo VGA fallback por simplicidad.
 *
 * Limitaciones conocidas (intencionales para esta fase):
 *   - Scroll hace memcpy del framebuffer entero (~3 MB en 1024x768x32);
 *     es lento (~30-50 ms en QEMU) pero correcto. Se puede optimizar luego.
 *   - No hay double buffering: cada putchar pinta directo al FB.
 *   - Solo soporta RGB tipo 1 con bpp=32 layout XRGB8888 (estandar QEMU).
 */
#include "fb.h"
#include "../kernel/multiboot.h"
#include "../lib/printf.h"

/* Tabla de colores VGA -> RGB888.
 * Indice = numero de color VGA (0-15).
 * Valor = 0xRRGGBB (los 8 bits altos se ignoran, se rellenan a 0 en XRGB). */
static const uint32_t vga_palette[16] = {
    0x000000, /* 0 black */
    0x0000AA, /* 1 blue */
    0x00AA00, /* 2 green */
    0x00AAAA, /* 3 cyan */
    0xAA0000, /* 4 red */
    0xAA00AA, /* 5 magenta */
    0xAA5500, /* 6 brown */
    0xAAAAAA, /* 7 light_grey */
    0x555555, /* 8 dark_grey */
    0x5555FF, /* 9 light_blue */
    0x55FF55, /* 10 light_green */
    0x55FFFF, /* 11 light_cyan */
    0xFF5555, /* 12 light_red */
    0xFF55FF, /* 13 light_magenta */
    0xFFFF55, /* 14 yellow */
    0xFFFFFF, /* 15 white */
};

/* Fuente 8x8 esta definida en drivers/vga.c. La compartimos. */
extern const uint8_t vga_font_8x8[2048];

#define CELL_W 8
#define CELL_H 8
#define TAB_WIDTH 4

/* Estado del driver. */
static bool     active = false;
static uint8_t *fb_ptr;          /* phys addr del framebuffer */
static uint32_t fb_pitch;        /* bytes por scanline */
static int      fb_width;        /* pixels */
static int      fb_height;
static int      fb_bpp;
static int      cols, rows;      /* cells de texto */
static int      cur_col, cur_row;
static uint8_t  cur_color;       /* mismo formato VGA: (bg<<4)|fg */

bool fb_is_active(void)    { return active; }
int  fb_text_cols(void)    { return cols; }
int  fb_text_rows(void)    { return rows; }
int  fb_pixel_width(void)  { return fb_width; }
int  fb_pixel_height(void) { return fb_height; }
uint32_t fb_get_addr(void)  { return (uint32_t)(uintptr_t)fb_ptr; }
uint32_t fb_get_pitch(void) { return fb_pitch; }
int      fb_get_bpp(void)   { return fb_bpp; }

bool fb_init(uint32_t mb_magic, uint32_t mb_info_addr)
{
    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC) return false;
    multiboot_info_t *mbi = (multiboot_info_t *)mb_info_addr;

    /* GRUB tiene que habernos dado framebuffer (flag bit 12). */
    if (!(mbi->flags & MULTIBOOT_FLAG_FB)) return false;

    /* Solo soportamos RGB lineal (type 1). El tipo 2 (EGA text) significa
     * que GRUB nos puso en modo texto en lugar de grafico — fallback a VGA. */
    if (mbi->framebuffer_type != MULTIBOOT_FB_TYPE_RGB) return false;

    /* Solo soportamos bpp = 32 (RGBA) para esta primera version. */
    if (mbi->framebuffer_bpp != 32) return false;

    /* El framebuffer suele estar en addr alta (~0xFD000000 en QEMU).
     * Hay que asegurarse de que cae en el identity map de 256 MB del kernel
     * — si no, el kernel falla al escribir. QEMU pone el fb tipicamente en
     * 0xFD000000 que NO esta mapeado, asi que necesitamos detectarlo. */
    uint64_t fb_addr64 = mbi->framebuffer_addr;
    if (fb_addr64 >> 32) {
        /* addr > 4 GB, imposible en x86-32 sin PAE */
        return false;
    }
    uint32_t fb_addr = (uint32_t)fb_addr64;

    /* Cuanto framebuffer hay que mapear */
    uint32_t fb_size = mbi->framebuffer_pitch * mbi->framebuffer_height;

    /* Si el FB esta fuera del identity-map (256 MB), hay que mapearlo
     * explicitamente. Esto requiere vmm. */
    extern void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
    if (fb_addr >= 0x10000000u) {
        /* Mapeo identity: virt = phys (para que el kernel pueda escribir
         * usando la direccion fisica). Cada pagina mapeada PRESENT|RW. */
        for (uint32_t off = 0; off < fb_size; off += 4096) {
            vmm_map_page(fb_addr + off, fb_addr + off, 0x3);
        }
    }

    fb_ptr    = (uint8_t *)(uintptr_t)fb_addr;
    fb_pitch  = mbi->framebuffer_pitch;
    fb_width  = (int)mbi->framebuffer_width;
    fb_height = (int)mbi->framebuffer_height;
    fb_bpp    = (int)mbi->framebuffer_bpp;
    cols      = fb_width / CELL_W;
    rows      = fb_height / CELL_H;
    cur_col   = 0;
    cur_row   = 0;
    cur_color = 0x07;  /* light_grey on black */
    active    = true;

    fb_clear();
    return true;
}

void fb_put_pixel(int x, int y, uint32_t rgb)
{
    if (!active) return;
    if ((unsigned)x >= (unsigned)fb_width)  return;
    if ((unsigned)y >= (unsigned)fb_height) return;
    /* bpp=32, layout XRGB8888 little-endian:
     *   byte[0] = B, byte[1] = G, byte[2] = R, byte[3] = X */
    uint32_t *row = (uint32_t *)(fb_ptr + (uint32_t)y * fb_pitch);
    row[x] = rgb;
}

uint32_t fb_get_pixel(int x, int y)
{
    if (!active) return 0;
    if ((unsigned)x >= (unsigned)fb_width)  return 0;
    if ((unsigned)y >= (unsigned)fb_height) return 0;
    uint32_t *row = (uint32_t *)(fb_ptr + (uint32_t)y * fb_pitch);
    return row[x];
}

/* Pinta un glifo de 8x8 pixels en (cx,cy) cell coords. */
static void blit_glyph(int cx, int cy, char c, uint8_t color)
{
    uint8_t fg = color & 0x0F;
    uint8_t bg = (color >> 4) & 0x0F;
    uint32_t fg_rgb = vga_palette[fg];
    uint32_t bg_rgb = vga_palette[bg];
    const uint8_t *glyph = &vga_font_8x8[(unsigned char)c * 8];

    int px = cx * CELL_W;
    int py = cy * CELL_H;

    for (int row = 0; row < CELL_H; row++) {
        uint8_t bits = glyph[row];
        uint32_t *line = (uint32_t *)(fb_ptr + (uint32_t)(py + row) * fb_pitch);
        for (int col = 0; col < CELL_W; col++) {
            line[px + col] = (bits & 0x80) ? fg_rgb : bg_rgb;
            bits <<= 1;
        }
    }
}

void fb_clear(void)
{
    if (!active) return;
    uint8_t bg = (cur_color >> 4) & 0x0F;
    uint32_t rgb = vga_palette[bg];
    /* Llenar el framebuffer entero. Lento (~3 MB para 1024x768) pero correcto. */
    for (int y = 0; y < fb_height; y++) {
        uint32_t *line = (uint32_t *)(fb_ptr + (uint32_t)y * fb_pitch);
        for (int x = 0; x < fb_width; x++) line[x] = rgb;
    }
    cur_col = 0;
    cur_row = 0;
}

void fb_set_color(uint8_t color) { cur_color = color; }

void fb_set_cursor(int col, int row)
{
    if (col < 0) col = 0; if (col >= cols) col = cols - 1;
    if (row < 0) row = 0; if (row >= rows) row = rows - 1;
    cur_col = col;
    cur_row = row;
}

void fb_get_cursor(int *col, int *row)
{
    if (col) *col = cur_col;
    if (row) *row = cur_row;
}

void fb_scroll(void)
{
    if (cur_row < rows) return;

    /* Subir todo el contenido una fila de cells (= CELL_H pixels). */
    int shift = CELL_H;
    int copy_h = fb_height - shift;
    /* memcpy de las lineas (skip top, dst=top, src=top+shift) */
    for (int y = 0; y < copy_h; y++) {
        uint8_t *dst = fb_ptr + (uint32_t)y * fb_pitch;
        uint8_t *src = fb_ptr + (uint32_t)(y + shift) * fb_pitch;
        /* copia byte por byte; fb_pitch puede ser > width*4 (padding) */
        for (uint32_t i = 0; i < fb_pitch; i++) dst[i] = src[i];
    }
    /* Limpiar la ultima fila de cells. */
    uint8_t bg = (cur_color >> 4) & 0x0F;
    uint32_t rgb = vga_palette[bg];
    for (int y = copy_h; y < fb_height; y++) {
        uint32_t *line = (uint32_t *)(fb_ptr + (uint32_t)y * fb_pitch);
        for (int x = 0; x < fb_width; x++) line[x] = rgb;
    }
    cur_row = rows - 1;
}

void fb_putchar(char c)
{
    if (!active) return;
    switch (c) {
    case '\n':
        cur_col = 0;
        cur_row++;
        break;
    case '\r':
        cur_col = 0;
        break;
    case '\t':
        cur_col = (cur_col + TAB_WIDTH) & ~(TAB_WIDTH - 1);
        if (cur_col >= cols) { cur_col = 0; cur_row++; }
        break;
    case '\b':
        if (cur_col > 0) cur_col--;
        else if (cur_row > 0) { cur_row--; cur_col = cols - 1; }
        blit_glyph(cur_col, cur_row, ' ', cur_color);
        break;
    default:
        blit_glyph(cur_col, cur_row, c, cur_color);
        cur_col++;
        if (cur_col >= cols) { cur_col = 0; cur_row++; }
        break;
    }
    fb_scroll();
}
