#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    df_info_t d; if(dfinfo_(&d)<0){print("df: error\n");return 1;}
    if(!d.have_disk){print("No disk. RAM only.\n");return 0;}
    print("Filesystem  Blocks  Used    Free    BlockSize\n");
    print("/dev/sda    "); print_unum(d.total_blocks); print("  ");
    print_unum(d.used_blocks); print("  "); print_unum(d.total_blocks-d.used_blocks); print("  ");
    print_unum(d.block_size); print("\n");
    return 0;
}
