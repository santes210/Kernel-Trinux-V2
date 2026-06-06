#ifndef KERNEL_MULTIBOOT_H
#define KERNEL_MULTIBOOT_H

#include "../lib/types.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

typedef struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;     /* KiB below 1 MiB */
    uint32_t mem_upper;     /* KiB above 1 MiB */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t bootloader_name;
    uint32_t apm_table;
    /* ---- VBE / VESA info (flag bit 11 = 0x800) ---- */
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    /* ---- Framebuffer info (flag bit 12 = 0x1000) ----
     * Llenado por GRUB cuando el kernel pide FB_VIDMODE en su header. */
    uint64_t framebuffer_addr;        /* phys address del framebuffer lineal */
    uint32_t framebuffer_pitch;       /* BYTES por scanline (NO width*bpp) */
    uint32_t framebuffer_width;       /* pixels */
    uint32_t framebuffer_height;      /* pixels */
    uint8_t  framebuffer_bpp;         /* bits per pixel: 8, 16, 24, 32 */
    uint8_t  framebuffer_type;        /* 0=indexed, 1=RGB, 2=EGA text */
    uint8_t  framebuffer_color_info[6];
} __attribute__((packed)) multiboot_info_t;

typedef struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;          /* 1 = available */
} __attribute__((packed)) multiboot_mmap_entry_t;

#define MULTIBOOT_FLAG_MEM  0x001
#define MULTIBOOT_FLAG_MMAP 0x040
#define MULTIBOOT_FLAG_VBE  0x800        /* bit 11: vbe_* fields valid */
#define MULTIBOOT_FLAG_FB   0x1000       /* bit 12: framebuffer_* fields valid */
#define MULTIBOOT_MEMORY_AVAILABLE 1

#define MULTIBOOT_FB_TYPE_INDEXED 0
#define MULTIBOOT_FB_TYPE_RGB     1
#define MULTIBOOT_FB_TYPE_EGA     2

#endif /* KERNEL_MULTIBOOT_H */
