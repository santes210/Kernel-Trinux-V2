#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: dirname <path>\n");return 1;}
    char buf[256]; int n=0,ls=-1;
    for(int i=0;argv[1][i];i++){if(argv[1][i]=='/') ls=i; buf[n++]=argv[1][i];}
    buf[n]=0;
    if(ls<0){print(".\n");return 0;}
    if(ls==0){print("/\n");return 0;}
    buf[ls]=0; println(buf);
    return 0;
}
