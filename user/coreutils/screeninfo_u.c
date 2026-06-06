#include "../trinux.h"
static void print_hex32(uint32_t v) {
    const char *hex = "0123456789abcdef";
    char buf[9]; int i = 7; buf[8] = 0;
    while (i >= 0) { buf[i--] = hex[v & 0xF]; v >>= 4; }
    print("0x"); print(buf);
}
int main(int argc, char **argv){(void)argc;(void)argv;
    fb_info_t info;
    if (fb_info(&info) < 0) {print("screeninfo: SYS_FB_INFO no soportado\n");return 1;}
    print("Display mode:        ");
    print(info.active ? "GRAPHICS (VBE framebuffer)\n" : "TEXT (VGA legacy)\n");
    print("Text grid:           "); print_num(info.text_cols); print(" cols x ");
    print_num(info.text_rows); print(" rows\n");
    if (info.active) {
        print("Resolution:          "); print_num(info.width); print(" x "); print_num(info.height);
        print(" pixels, "); print_num(info.bpp); print(" bpp\n");
        print("Framebuffer addr:    "); print_hex32(info.fb_addr); print("\n");
        print("Pitch:               "); print_num((int)info.pitch); print(" bytes/scanline\n");
        uint32_t fb_size = info.pitch * info.height;
        print("Framebuffer size:    "); print_num((int)(fb_size / 1024)); print(" KiB\n");
        print("\nMatched profile:     ");
        if (info.width == 1366 && info.height == 768) print("HP Stream 14 (1366x768 nativo)\n");
        else if (info.width == 1280 && info.height == 800) print("QEMU default (1280x800)\n");
        else if (info.width == 1024 && info.height == 768) print("XGA fallback (1024x768)\n");
        else if (info.width == 1920 && info.height == 1080) print("Full HD (1920x1080)\n");
        else if (info.width == 800 && info.height == 600) print("SVGA fallback (800x600)\n");
        else print("custom\n");
    } else {
        print("\nNote: estas en modo texto VGA. Posibles causas:\n");
        print("  - GRUB no ofrecio framebuffer (BIOS muy vieja o UEFI puro)\n");
        print("  - El bpp pedido (32) no esta disponible en tu adaptador\n");
    }
    return 0;
}
