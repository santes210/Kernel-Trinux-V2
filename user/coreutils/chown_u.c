#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: chown <uid[:gid]> <file>\n");return 1;}
    int uid=ai(argv[1]), gid=uid;
    for(int i=0;argv[1][i];i++) if(argv[1][i]==':'){gid=ai(argv[1]+i+1);break;}
    if(chown_(argv[2], uid, gid)<0){print("chown: failed\n");return 1;}
    return 0;
}
