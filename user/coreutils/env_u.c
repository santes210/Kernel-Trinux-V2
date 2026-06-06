#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    char b[64];
    getuser(b,sizeof(b)); print("USER="); println(b);
    hostname(b,sizeof(b)); print("HOSTNAME="); println(b);
    char cwd[256]; getcwd(cwd,sizeof(cwd));
    print("PWD="); println(cwd);
    print("PATH=/bin\nSHELL=/bin/sh\nTERM=trinux-vga\n");
    return 0;
}
