#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v; if(sys_shutdown()<0){print("shutdown: permission denied\n");return 1;} return 0;}
