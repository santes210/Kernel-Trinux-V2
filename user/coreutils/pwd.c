#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v; char b[256]; if(getcwd(b,sizeof(b))<0){print("pwd: error\n");return 1;} println(b); return 0;}
