#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: color <fg> <bg> (0-15)\n");return 1;}
    vga_color(ai(argv[1]),ai(argv[2]));
    return 0;
}
