#include "../trinux.h"
static char buf[2048];
int main(int argc, char **argv){
    if(argc<2){print("usage: write <file>\n");return 1;}
    int total=0;
    for(;;){
        char line[256]; int n=getline_(line,sizeof(line));
        if(n<=0) break;
        for(int i=0;i<n&&total<(int)sizeof(buf)-2;i++) buf[total++]=line[i];
        buf[total++]='\n';
    }
    unlink(argv[1]);
    if(writefile(argv[1],buf,total)<0){print("write: failed\n");return 1;}
    return 0;
}
