#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v; char b[64]; if(getuser(b,sizeof(b))<0){print("?\n");return 1;} println(b); return 0;}
