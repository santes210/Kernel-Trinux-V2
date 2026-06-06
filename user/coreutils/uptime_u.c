#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v; print("uptime: "); print_unum(uptime()); print(" seconds\n"); return 0;}
