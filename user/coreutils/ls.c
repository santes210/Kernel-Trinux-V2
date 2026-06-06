#include "../trinux.h"
static void pperm(uint32_t p, uint32_t t) {
    putchar_(t==2?'d':(t==3?'c':'-'));
    putchar_(p&0400?'r':'-'); putchar_(p&0200?'w':'-'); putchar_(p&0100?'x':'-');
    putchar_(p&0040?'r':'-'); putchar_(p&0020?'w':'-'); putchar_(p&0010?'x':'-');
    putchar_(p&0004?'r':'-'); putchar_(p&0002?'w':'-'); putchar_(p&0001?'x':'-');
}
int main(int argc, char **argv) {
    int lf = 0; const char *path = ".";
    for (int i=1;i<argc;i++) { if (streq(argv[i],"-l")) lf=1; else path=argv[i]; }
    int dh = opendir(path);
    if (dh < 0) { print("ls: cannot open "); print(path); print("\n"); return 1; }
    trinux_dirent_t de;
    while (readdir(dh,&de)==0) {
        if (lf) {
            char child[256]; int li=0;
            while (path[li] && li<240) { child[li]=path[li]; li++; }
            if (li>0 && child[li-1]!='/') child[li++]='/';
            int j=0; while (de.name[j] && li<255) { child[li++]=de.name[j++]; }
            child[li]=0;
            trinux_stat_t st;
            if (stat(child,&st)==0) { pperm(st.perm,st.type); print("  "); print_unum(st.size); print("  "); }
            else print("?????????  ?  ");
        }
        if (de.type==2) { print(de.name); print("/"); } else print(de.name);
        print(lf?"\n":"  ");
    }
    closedir(dh);
    if (!lf) print("\n");
    return 0;
}
