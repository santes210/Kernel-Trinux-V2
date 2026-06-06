#include "../trinux.h"
static char buf[2048];
int main(int argc, char **argv){
    int append=0; const char *file=0;
    for(int i=1;i<argc;i++){if(streq(argv[i],"-a"))append=1; else file=argv[i];}
    if(!file){print("usage: tee [-a] <file>\n");return 1;}
    int n=getline_(buf,sizeof(buf));
    print(buf); print("\n");
    if(append){
        static char old[4096]; int o=readfile(file,old,sizeof(old)); if(o<0)o=0;
        for(int i=0;i<n&&o+i<(int)sizeof(old)-2;i++) old[o++]=buf[i];
        old[o++]='\n';
        unlink(file);
        writefile(file,old,o);
    } else {
        buf[n]='\n';
        unlink(file);
        writefile(file,buf,n+1);
    }
    return 0;
}
