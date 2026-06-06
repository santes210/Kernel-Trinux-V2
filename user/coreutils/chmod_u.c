#include "../trinux.h"
static int oct(const char *s){int n=0;while(*s>='0'&&*s<='7'){n=n*8+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: chmod <octal> <file>\n");return 1;}
    if(chmod_(argv[2], oct(argv[1]))<0){print("chmod: failed\n");return 1;}
    return 0;
}
