#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    mem_info_t m; if(meminfo_(&m)<0){print("free: error\n");return 1;}
    print("               total       used       free\n");
    print("Mem (bytes):  "); print_unum(m.total_bytes); print("  ");
    print_unum(m.used_bytes); print("  "); print_unum(m.free_bytes); print("\n");
    print("Mem (KiB)  :  "); print_unum(m.total_bytes/1024); print("        ");
    print_unum(m.used_bytes/1024); print("        "); print_unum(m.free_bytes/1024); print("\n");
    return 0;
}
