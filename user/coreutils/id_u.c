#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v;
    char u[64]; getuser(u,sizeof(u));
    print("uid="); print_num(getuid()); print("("); print(u); print(")\n"); return 0;
}
