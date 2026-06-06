#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){char b[64]; hostname(b,sizeof(b)); println(b); return 0;}
    int n=strlen_(argv[1]);
    if(writefile("/etc/hostname", argv[1], n)<0){print("cannot write\n");return 1;}
    print("/etc/hostname updated. Reboot to apply.\n");
    return 0;
}
