#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc,char**argv){
    if(argc<2){print("usage: sleep <seconds>\n");return 1;}
    msleep(ai(argv[1])*1000); return 0;
}
