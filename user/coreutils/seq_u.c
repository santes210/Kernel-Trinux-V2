#include "../trinux.h"
static int ai(const char*s){int n=0,sg=1;if(*s=='-'){sg=-1;s++;} while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;} return sg*n;}
int main(int argc, char **argv){
    int a=1,b=0;
    if(argc==2){a=1; b=ai(argv[1]);}
    else if(argc>=3){a=ai(argv[1]); b=ai(argv[2]);}
    else {print("usage: seq [start] end\n"); return 1;}
    for(int i=a;i<=b;i++){print_num(i); print("\n");}
    return 0;
}
