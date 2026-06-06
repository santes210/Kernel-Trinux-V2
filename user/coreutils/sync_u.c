#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v;
    if(sys_sync()<0){print("sync: failed (no disk?)\n");return 1;}
    print("Filesystem synced to disk.\n"); return 0;
}
