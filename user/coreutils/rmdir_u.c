#include "../trinux.h"
int main(int argc,char**argv){
    if(argc<2){print("usage: rmdir <dir>...\n");return 1;}
    int rc=0;
    for(int i=1;i<argc;i++) if(rmdir(argv[i])<0){print("rmdir: ");print(argv[i]);print(": failed\n");rc=1;}
    return rc;
}
