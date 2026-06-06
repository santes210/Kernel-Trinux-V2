#include "../trinux.h"
int main(int argc, char **argv){
    if(argc < 2){
        print("usage: tcc <source.c>\n");
        print("  Compila .c -> ELF ring 3 (mismo nombre sin .c)\n");
        print("  Helpers: print, print_num, getchar, sleep, uptime, getpid,\n");
        print("           exit, vga_clear, vga_putchar, vga_print\n");
        return 1;
    }
    int rc = tcc_compile(argv[1]);
    if(rc < 0){print("tcc: compilation failed\n"); return 1;}
    char out[128]; int i=0;
    while(argv[1][i] && i<127){out[i]=argv[1][i]; i++;} out[i] = 0;
    if(i > 2 && out[i-2]=='.' && out[i-1]=='c') out[i-2] = 0;
    print("tcc: compiled -> "); println(out);
    return 0;
}
