/* kernel/kernel.c  -  kernel entry and subsystem initialization. */
#include "../include/kernel.h"
#include "multiboot.h"
#include "../drivers/vga.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../cpu/idt.h"
#include "../cpu/isr.h"
#include "../cpu/irq.h"
#include "../cpu/syscall.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/kheap.h"
#include "../fs/vfs.h"
#include "../fs/ramfs.h"
#include "../fs/diskfs.h"
#include "../fs/blockfs.h"
#include "../fs/devfs.h"
#include "../drivers/ata.h"
#include "../auth/users.h"
#include "../process/process.h"
#include "../process/scheduler.h"
#include "../lib/printf.h"
#include "../shell/shell.h"

/* Check if a word appears in a space-separated string. */
static bool cmdline_has(const char *cmdline, const char *word)
{
    if (!cmdline || !word) return false;
    int wlen = 0;
    while (word[wlen]) wlen++;

    const char *p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - start);
        if (len == wlen) {
            bool match = true;
            for (int i = 0; i < wlen; i++)
                if (start[i] != word[i]) { match = false; break; }
            if (match) return true;
        }
    }
    return false;
}

static uint32_t detect_memory(uint32_t magic, uint32_t mb_addr)
{
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        return 16 * 1024 * 1024;   /* fallback 16 MiB */

    multiboot_info_t *mbi = (multiboot_info_t *)mb_addr;
    if (mbi->flags & MULTIBOOT_FLAG_MEM) {
        /* mem_upper is KiB above 1 MiB */
        return (mbi->mem_upper * 1024) + (1024 * 1024);
    }
    return 16 * 1024 * 1024;
}

static void print_banner(uint32_t total_mem)
{
    vga_set_color(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    kprintf("\n");
    kprintf("  =============================================\n");
    kprintf("   %s v%s  (%s)\n", KERNEL_NAME, KERNEL_VERSION, KERNEL_ARCH);
    kprintf("   %s\n", KERNEL_BUILD);
    kprintf("  =============================================\n");
    vga_set_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    kprintf("  Memory detected : %u KiB (%u MiB)\n",
            total_mem / 1024, total_mem / (1024 * 1024));
}

static void ok(const char *what)
{
    vga_set_color(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    kprintf("  [ OK ] ");
    vga_set_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    kprintf("%s\n", what);
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    serial_init();
    serial_write("[boot] kernel_main entered\n");

    vga_init();

    uint32_t total_mem = detect_memory(magic, mb_info_addr);
    print_banner(total_mem);

    /* Interrupts */
    idt_install();           ok("IDT installed");
    isr_install();           ok("CPU exception handlers (ISR 0-31)");
    irq_install();           ok("PIC remapped, IRQ handlers ready");
    syscall_install();       ok("Syscall gate int 0x80 (ring 3 -> ring 0)");

    /* Memory */
    kheap_init();            ok("Kernel heap (32 MiB arena)");
    pmm_init(total_mem);     ok("Physical memory manager");
    vmm_init();              ok("Paging enabled (identity map 256 MiB)");

    /* Devices */
    timer_init(100);         ok("PIT timer @ 100 Hz");
    keyboard_init();         ok("PS/2 keyboard (US QWERTY)");

    /* Filesystem */
    vfs_init();
    ramfs_init();            ok("RAM filesystem mounted at /");

    /* Disk persistence: probe the ATA disk and, if a saved image exists,
     * restore the tree from it (replacing the default ramfs contents). */
    if (ata_init()) {
        uint32_t disk_mb = diskfs_total_mb();
        char dmsg[80];
        /* Show which backend the disk driver is using. */
        extern bool ahci_present(void);
        extern bool xhci_present(void);
        const char *mode = xhci_present() ? "xHCI/USB" :
                           ahci_present() ? "AHCI/SATA" : "IDE PIO";
        snprintf(dmsg, sizeof(dmsg),
                 "Disk detected via %s (%u MiB)", mode, disk_mb);
        ok(dmsg);
        if (blockfs_init())
            ok("Block storage ready (disk-backed files)");
        int r = diskfs_load();
        if (r == 1)
            ok("Filesystem restored from disk");
        else if (r == 0)
            ok("No saved image; using fresh filesystem (use 'sync' to save)");
        else
            ok("Disk present but image unreadable; using fresh filesystem");
    } else {
        ok("No ATA disk (filesystem is RAM-only this session)");
    }

    /* Populate /dev with character devices (/dev/sda, zero, null, random).
     * Must come after ata_init() so /dev/sda is created when a disk exists. */
    devfs_init();            ok("Device files in /dev (sda, zero, null, random)");

    extern void fat16_init(void);
    fat16_init();

    /* Users (reads /etc/passwd which may now come from disk) */
    users_init();            ok("User accounts (/etc/passwd, /etc/shadow)");

    /* Processes */
    process_init();
    scheduler_init();        ok("Process table + scheduler");

    /* Enable interrupts and launch the shell. */
    __asm__ volatile("sti");
    ok("Interrupts enabled");

    /* Parse kernel command line from GRUB (multiboot).
     * Recognized options:
     *   init=/bin/bash  — drop to root shell without login
     *   single          — same as init=/bin/bash
     *   emergency       — same as init=/bin/bash
     *
     * To use on real hardware: at the GRUB menu, press 'e', add the
     * option to the 'multiboot' line, and press Ctrl-X to boot. */
    const char *cmdline = NULL;
    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        multiboot_info_t *mbi = (multiboot_info_t *)mb_info_addr;
        if ((mbi->flags & (1u << 2)) && mbi->cmdline)
            cmdline = (const char *)(uintptr_t)mbi->cmdline;
    }

    int boot_mode = 0;
    if (cmdline && (cmdline_has(cmdline, "init=/bin/bash") ||
                    cmdline_has(cmdline, "init=/bin/sh") ||
                    cmdline_has(cmdline, "single") ||
                    cmdline_has(cmdline, "emergency"))) {
        boot_mode = 1;
        ok("Kernel cmdline: single-user mode requested");
    }

    extern void shell_set_boot_mode(int mode);
    shell_set_boot_mode(boot_mode);

    vga_set_color(vga_entry_color(VGA_WHITE, VGA_BLACK));
    kprintf("\n  Type 'help' for a list of commands.\n");
    if (boot_mode == 0)
        kprintf("  (To reset passwords: reboot, press 'e' at GRUB,\n"
                "   add 'single' to the multiboot line, press Ctrl-X)\n");
    kprintf("\n");
    vga_set_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    shell_run();

    /* Should never return. */
    for (;;)
        __asm__ volatile("hlt");
}
