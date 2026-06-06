#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: useradd <name> <pass> [uid] [gid] [home]\n");return 1;}
    useradd_req_t r;
    r.name=argv[1]; r.pass=argv[2];
    r.uid = argc>=4 ? ai(argv[3]) : 1001;
    r.gid = argc>=5 ? ai(argv[4]) : 1001;
    r.home = argc>=6 ? argv[5] : "/home/user";
    if(useradd_(&r)<0){print("useradd: failed (need root)\n");return 1;}
    print("user added: "); print(argv[1]); print("\n");
    return 0;
}
