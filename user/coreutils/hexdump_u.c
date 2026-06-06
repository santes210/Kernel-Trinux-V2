#include "../trinux.h"
static char buf[4096];
static void h2(int v){const char*h="0123456789abcdef"; putchar_(h[(v>>4)&15]); putchar_(h[v&15]);}
static void h8(uint32_t v){const char*h="0123456789abcdef"; for(int i=28;i>=0;i-=4) putchar_(h[(v>>i)&15]);}
int main(int argc, char **argv){
    if(argc<2){print("usage: hexdump <file>\n");return 1;}
    int n=readfile(argv[1],buf,sizeof(buf));
    if(n<0){print("hexdump: no such file\n");return 1;}
    for(int off=0;off<n;off+=16){
        h8(off); print("  ");
        for(int i=0;i<16;i++){
            if(off+i<n){h2((uint8_t)buf[off+i]); putchar_(' ');} else print("   ");
            if(i==7) putchar_(' ');
        }
        print(" |");
        for(int i=0;i<16&&off+i<n;i++){uint8_t c=(uint8_t)buf[off+i]; putchar_((c>=32&&c<127)?c:'.');}
        print("|\n");
    }
    return 0;
}
