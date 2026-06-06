#include "../trinux.h"
static char buf[8192];
int main(int argc,char**argv){
    if(argc<2){print("usage: wc <file>...\n");return 1;}
    int rc=0;
    for(int i=1;i<argc;i++){
        int n=readfile(argv[i],buf,sizeof(buf));
        if(n<0){print("wc: ");print(argv[i]);print(": no such file\n");rc=1;continue;}
        int L=0,W=0,iw=0;
        for(int k=0;k<n;k++){char c=buf[k]; if(c=='\n')L++;
            if(c==' '||c=='\t'||c=='\n'){if(iw){W++;iw=0;}} else iw=1;}
        if(iw)W++;
        print(" ");print_num(L);print(" ");print_num(W);print(" ");print_num(n);
        print(" ");print(argv[i]);print("\n");
    }
    return rc;
}
