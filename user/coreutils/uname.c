#include "../trinux.h"
int main(int argc,char**argv){
    int a=0; for(int i=1;i<argc;i++) if(streq(argv[i],"-a")) a=1;
    if(a){char h[64]; hostname(h,sizeof(h)); print("Trinux "); print(h); print(" 0.2.1 i686\n");}
    else print("Trinux\n");
    return 0;
}
