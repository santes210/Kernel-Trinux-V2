#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    char u[64],h[64]; getuser(u,sizeof(u)); hostname(h,sizeof(h));
    mem_info_t mi; meminfo_(&mi);
    df_info_t di; dfinfo_(&di);
    print("      .--.        ");
    vga_color(11,0); print(u); vga_color(7,0); print("@"); vga_color(11,0); print(h); vga_color(7,0); print("\n");
    print("     |o_o |       OS:     Trinux 0.2.3 (ring 3 shell)\n");
    print("     |:_/ |       Kernel: x86 32-bit protected mode\n");
    print("    //   \\ \\      Uptime: "); print_unum(uptime()); print(" s\n");
    print("   (|     | )     Shell:  /bin/sh (PID="); print_num(getpid()); print(")\n");
    print("  /'\\_   _/`\\     Memory: ");
    print_unum(mi.used_bytes/(1024*1024)); print("/"); print_unum(mi.total_bytes/(1024*1024)); print(" MiB\n");
    print("  \\___)=(___/     Disk:   ");
    if(di.have_disk){print_unum(di.used_blocks*(di.block_size/1024)); print("/");
        print_unum(di.total_blocks*(di.block_size/1024)); print(" KiB\n");}
    else print("RAM only\n");
    return 0;
}
