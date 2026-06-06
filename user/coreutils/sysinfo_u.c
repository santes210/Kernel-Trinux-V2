#include "../trinux.h"
#define LIGHT_GREY 7
#define LIGHT_GREEN 10
#define LIGHT_CYAN 11
#define YELLOW 14
int main(int argc, char **argv){(void)argc;(void)argv;
    vga_color(LIGHT_CYAN, 0);
    print("================================================\n");
    print("  Trinux System Info\n");
    print("================================================\n");
    vga_color(LIGHT_GREY, 0);
    smp_info_t cpu;
    if (smp_info(&cpu) == 0) {
        vga_color(YELLOW, 0); print("\n[CPU]\n"); vga_color(LIGHT_GREY, 0);
        print("  Cores detected:  "); print_num(cpu.n_cpus); print("\n");
        print("  Cores online:    "); print_num(cpu.online);
        if (cpu.n_cpus > 1 && cpu.online == 1) print(" (BSP only)");
        print("\n");
        print("  BSP APIC ID:     "); print_num(cpu.bsp_apic_id); print("\n");
    }
    fb_info_t fb;
    if (fb_info(&fb) == 0) {
        vga_color(YELLOW, 0); print("\n[Display]\n"); vga_color(LIGHT_GREY, 0);
        if (fb.active) {
            print("  Mode:            graphics (VBE)\n");
            print("  Resolution:      "); print_num(fb.width); print(" x "); print_num(fb.height);
            print(" @ "); print_num(fb.bpp); print(" bpp\n");
        } else {
            print("  Mode:            text VGA\n");
        }
        print("  Text grid:       "); print_num(fb.text_cols); print(" x "); print_num(fb.text_rows); print("\n");
    }
    mem_info_t mem;
    if (meminfo_(&mem) == 0) {
        vga_color(YELLOW, 0); print("\n[Memory]\n"); vga_color(LIGHT_GREY, 0);
        print("  Total:           "); print_unum(mem.total_bytes / (1024*1024)); print(" MiB\n");
        print("  Used:            "); print_unum(mem.used_bytes / (1024*1024)); print(" MiB\n");
        print("  Free:            "); print_unum(mem.free_bytes / (1024*1024)); print(" MiB\n");
    }
    df_info_t df;
    if (dfinfo_(&df) == 0) {
        vga_color(YELLOW, 0); print("\n[Storage]\n"); vga_color(LIGHT_GREY, 0);
        if (df.have_disk) {
            print("  Disk:            present\n");
            print("  Total:           "); print_unum((df.total_blocks * df.block_size) / (1024*1024)); print(" MiB\n");
            print("  Used:            "); print_unum((df.used_blocks * df.block_size) / (1024*1024)); print(" MiB\n");
        } else print("  Disk:            none (RAM-only)\n");
    }
    battery_u_t bat;
    if (battery_(&bat) == 0 && bat.present) {
        vga_color(YELLOW, 0); print("\n[Battery]\n"); vga_color(LIGHT_GREY, 0);
        print("  Status:          ");
        if (bat.discharging) {vga_color(YELLOW, 0); print("discharging");}
        else {vga_color(LIGHT_GREEN, 0); print("charging or AC");}
        vga_color(LIGHT_GREY, 0); print("\n");
        print("  Charge:          "); print_num(bat.percent); print(" %\n");
    } else {
        vga_color(YELLOW, 0); print("\n[Battery]\n"); vga_color(LIGHT_GREY, 0);
        print("  No battery detected (AC-only / VM)\n");
    }
    vga_color(YELLOW, 0); print("\n[Time]\n"); vga_color(LIGHT_GREY, 0);
    print("  Uptime:          "); print_unum(uptime()); print(" seconds\n");
    char h[64]; hostname(h, sizeof(h));
    char u[64]; getuser(u, sizeof(u));
    vga_color(YELLOW, 0); print("\n[System]\n"); vga_color(LIGHT_GREY, 0);
    print("  Hostname:        "); print(h); print("\n");
    print("  Current user:    "); print(u); print(" (uid="); print_num(getuid()); print(")\n");
    print("  Shell PID:       "); print_num(getpid()); print("\n");
    vga_color(LIGHT_CYAN, 0);
    print("\n================================================\n");
    vga_color(LIGHT_GREY, 0);
    return 0;
}
