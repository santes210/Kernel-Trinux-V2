#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v; if(sys_reboot()<0){print("reboot: permission denied\n");return 1;} return 0;}
