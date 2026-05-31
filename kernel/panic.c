/* kernel/panic.c  -  kernel panic. */
#include "../include/kernel.h"
#include "../drivers/vga.h"
#include "../lib/printf.h"

void panic(const char *msg)
{
    __asm__ volatile("cli");
    vga_set_color(vga_entry_color(VGA_WHITE, VGA_RED));
    kprintf("\n=== KERNEL PANIC ===\n%s\nSystem halted.\n", msg);
    for (;;)
        __asm__ volatile("hlt");
}
