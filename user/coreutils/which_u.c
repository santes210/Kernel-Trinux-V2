#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: which <cmd>\n");return 1;}
    char path[128]="/bin/";
    int i=0; while(argv[1][i]&&i<120){path[5+i]=argv[1][i]; i++;}
    path[5+i]=0;
    trinux_stat_t st;
    if(stat(path,&st)<0){print(argv[1]); print(": not found\n"); return 1;}
    println(path);
    return 0;
}
