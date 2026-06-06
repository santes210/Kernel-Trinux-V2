#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<4){print("usage: calc <a> <op> <b>\n");return 1;}
    int a=ai(argv[1]), b=ai(argv[3]);
    int r=0;
    switch(argv[2][0]){
        case '+': r=a+b; break;
        case '-': r=a-b; break;
        case '*': case 'x': case 'X': r=a*b; break;
        case '/': r = b ? a/b : 0; break;
        default: print("calc: unknown op\n"); return 1;
    }
    print_num(r); print("\n");
    return 0;
}
