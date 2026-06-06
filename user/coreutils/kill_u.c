#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc,char**argv){
    if(argc<2){print("usage: kill <pid>\n");return 1;}
    if(sys_kill(ai(argv[1]),9)<0){print("kill: failed\n");return 1;}
    return 0;
}
