#include "../trinux.h"
static int ai(const char*s){int n=0,sg=1;if(*s=='-'){sg=-1;s++;} while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;} return sg*n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: renice <prio> <pid>\n");return 1;}
    if(renice_(ai(argv[2]), ai(argv[1]))<0){print("renice: failed\n");return 1;}
    return 0;
}
