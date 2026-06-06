#include "../trinux.h"
int main(int argc,char**argv){
    if(argc<2){print("usage: rm <file>...\n");return 1;}
    int rc=0;
    for(int i=1;i<argc;i++) if(unlink(argv[i])<0){print("rm: ");print(argv[i]);print(": failed\n");rc=1;}
    return rc;
}
