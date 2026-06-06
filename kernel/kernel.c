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
#include "elf.h"
/* Old V2.0 demo blobs (compilados con ABI de syscalls desactualizada).
 * Reemplazados por user/coreutils/user_bins.h.  Conservamos los .c sources
 * (tce/mini_test/slow) para `tcc`. */
#include "tce_src.h"
#include "mini_test_src.h"
#include "slow_src.h"
#include "../user/coreutils/user_bins.h"
#include "../user/usersh/sh_elf.h"

/* Install built-in ELF binaries into /bin if they don't already exist.
 * Called AFTER diskfs_load() so they survive regardless of disk state. */
static void install_builtin_apps(void)
{
    /* ensure /bin exists */
    vfs_node_t *bin = vfs_resolve("/bin", vfs_get_root());
    if (!bin)
        bin = vfs_mkdir("/bin", vfs_get_root());
    if (!bin) return;

    /* Drop los binarios coreutils ring-3 generados por user/coreutils/build.sh
     * en /bin/. SIEMPRE los sobreescribimos para asegurar que el ABI de
     * syscalls del kernel coincide con el de los binarios compilados
     * (si el usuario tenía versiones viejas en disco con números viejos
     * se reemplazan en cada boot). */
    for (unsigned i = 0; i < user_bins_count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bin/%s", user_bins[i].name);
        vfs_node_t *f = vfs_create(path, vfs_get_root());
        if (f) {
            f->permissions = 0755;
            f->size = 0;
            vfs_write(f, 0, *user_bins[i].size_p, (uint8_t *)user_bins[i].data);
        }
    }

    /* Y el shell userland (vive aparte) */
    vfs_node_t *fsh = vfs_create("/bin/sh", vfs_get_root());
    if (fsh) {
        fsh->permissions = 0755;
        fsh->size = 0;
        vfs_write(fsh, 0, u_sh_len, (uint8_t *)u_sh);
    }

    /* Drop the bundled source of `tce.c` into /root so the user can do:
     *     cd /root && tcc tce.c && exec tce
     * straight after boot.  Only created the first time (do nothing if
     * the user has already saved their own version to disk). */
    vfs_node_t *root_home = vfs_resolve("/root", vfs_get_root());
    if (!root_home)
        root_home = vfs_mkdir("/root", vfs_get_root());
    if (root_home && !vfs_finddir(root_home, "tce.c")) {
        vfs_node_t *src = vfs_create("/root/tce.c", vfs_get_root());
        if (src) {
            src->permissions = 0644;
            vfs_write(src, 0, tce_src_size, (uint8_t *)tce_src);
        }
    }
    if (root_home && !vfs_finddir(root_home, "mini_test.c")) {
        vfs_node_t *src = vfs_create("/root/mini_test.c", vfs_get_root());
        if (src) {
            src->permissions = 0644;
            vfs_write(src, 0, mini_test_src_size, (uint8_t *)mini_test_src);
        }
    }
    if (root_home && !vfs_finddir(root_home, "slow.c")) {
        vfs_node_t *src = vfs_create("/root/slow.c", vfs_get_root());
        if (src) {
            src->permissions = 0644;
            vfs_write(src, 0, slow_src_size, (uint8_t *)slow_src);
        }
    }
}

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

    /* Modo grafico (VBE/framebuffer) si GRUB lo ofrece. Necesita vmm
     * para mapear el framebuffer que esta en addr alta (> 256 MB). */
    if (display_init(magic, mb_info_addr)) {
        ok("Framebuffer (VBE) graphics mode active");
    } else {
        ok("Text mode VGA (no framebuffer offered by bootloader)");
    }

    /* SMP detection (Fase 1: solo lista cores, no los arranca).
     * Si la deteccion ACPI MADT falla, devuelve 1 (solo BSP). */
    {
        extern int  smp_detect(void);
        extern int  smp_cpu_count(void);
        extern uint32_t smp_lapic_base(void);
        int n = smp_detect();
        if (n > 1) {
            kprintf("  [ OK ] SMP: detected %d CPU cores (LAPIC @ %x), BSP only running\n",
                    n, smp_lapic_base());
        } else {
            ok("SMP: single-core (no ACPI MADT or only 1 CPU)");
        }
    }

    /* Devices */
    timer_init(100);         ok("PIT timer @ 100 Hz");
    keyboard_init();         ok("PS/2 keyboard (US QWERTY)");
    serial_enable_input();   ok("COM1 serial input (paste-friendly, 115200 8N1)");

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

    install_builtin_apps();  ok("Built-in apps installed in /bin");

    extern void fat16_init(void);
    fat16_init();

    /* Users (reads /etc/passwd which may now come from disk) */
    users_init();            ok("User accounts (/etc/passwd, /etc/shadow)");

    /* Processes */
    /* Order matters: scheduler_init() resets the run queue, and
     * process_init() registers the idle task with scheduler_add().
     * If we call process_init first the registration is wiped out
     * by the subsequent scheduler_init(). */
    scheduler_init();
    process_init();          ok("Process table + scheduler");

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

    /* Modo "oldsh": fuerza usar la shell vieja en ring 0 (debug). */
    int use_old_shell = (cmdline && cmdline_has(cmdline, "oldsh")) ? 1 : 0;

    vga_set_color(vga_entry_color(VGA_WHITE, VGA_BLACK));
    kprintf("\n  Type 'help' for a list of commands.\n");
    if (boot_mode == 0)
        kprintf("  (To reset passwords: reboot, press 'e' at GRUB,\n"
                "   add 'single' to the multiboot line, press Ctrl-X)\n");
    kprintf("\n");
    vga_set_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    if (use_old_shell) {
        kprintf("  [init] launching legacy ring-0 shell (oldsh)\n");
        shell_run();
    } else {
        /* === NUEVO: ejecutar /bin/sh en ring 3 como init. === */
        /* Necesitamos un shell_state vacío para que los syscalls que lo
         * usan (chdir, getcwd) tengan algo a lo que apuntar. */
        extern void shell_state_init_minimal(void);
        shell_state_init_minimal();

        /* Pre-loguear root si modo single, para que el shell ring-3 lo vea. */
        if (boot_mode == 1) {
            user_t *root = users_find("root");
            if (root) set_current_user(root);
        }

        kprintf("  [init] launching /bin/sh in RING 3 (CPL=3)\n");
        for (;;) {
            int rc = elf_exec("/bin/sh", vfs_get_root());
            kprintf("\n  [init] /bin/sh exited with code %d, restarting in 1s...\n", rc);
            /* FIX TERMICO: usar sleep() basado en timer (sti+hlt) en vez de
             * busy-loop que calentaba el CPU al 100% durante el delay.
             * Importante en hardware real (HP Stream 14 etc), nulo en QEMU. */
            extern void sleep(uint32_t ms);
            sleep(1000);
        }
    }

    /* Should never return. */
    for (;;)
        __asm__ volatile("hlt");
}
