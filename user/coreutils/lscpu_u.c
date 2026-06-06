#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    smp_info_t info;
    if (smp_info(&info) < 0) {print("lscpu: SYS_SMP_INFO no soportado\n");return 1;}
    print("Architecture:        i386 (x86 32-bit protected mode)\n");
    print("CPU(s):              "); print_num(info.n_cpus); print("\n");
    print("On-line CPU(s):      ");
    if (info.online == 1) print("0 (BSP only — APs detected but not started)\n");
    else { print("0-"); print_num(info.online-1); print("\n"); }
    print("BSP APIC ID:         "); print_num(info.bsp_apic_id); print("\n");
    print("LAPIC base address:  0x");
    char hexbuf[9]; const char *hex = "0123456789abcdef";
    uint32_t v = info.lapic_base; int i = 7; hexbuf[8] = 0;
    while (i >= 0) { hexbuf[i--] = hex[v & 0xF]; v >>= 4; }
    print(hexbuf); print("\n");
    print("\nDetected cores:\n  #    APIC ID    Status\n");
    for (int k = 0; k < info.n_cpus; k++) {
        print("  "); print_num(k); print("    "); print_num(info.apic_ids[k]);
        if (info.apic_ids[k] == info.bsp_apic_id) print("          RUNNING (BSP)\n");
        else print("          halted (AP, awaiting wake-up)\n");
    }
    return 0;
}
