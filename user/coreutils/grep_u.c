#include "../trinux.h"
static char buf[8192];
static int has(const char*h,const char*n){
    if(!*n)return 1;
    for(int i=0;h[i];i++){int j=0; while(n[j]&&h[i+j]==n[j])j++; if(!n[j])return 1;}
    return 0;
}
int main(int argc,char**argv){
    if(argc<3){print("usage: grep <pattern> <file>...\n");return 1;}
    const char*p=argv[1]; int rc=1;
    for(int f=2;f<argc;f++){
        int n=readfile(argv[f],buf,sizeof(buf)-1);
        if(n<0){print("grep: ");print(argv[f]);print(": no such file\n");continue;}
        buf[n]=0; char line[512]; int li=0;
        for(int i=0;i<=n;i++){
            char c=(i<n)?buf[i]:'\n';
            if(c=='\n'||li>=510){
                line[li]=0;
                if(has(line,p)){if(argc>3){print(argv[f]);print(":");} print(line);print("\n");rc=0;}
                li=0;
            } else line[li++]=c;
        }
    }
    return rc;
}
