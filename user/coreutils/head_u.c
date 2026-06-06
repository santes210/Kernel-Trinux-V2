#include "../trinux.h"
static char buf[8192];
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc,char**argv){
    int nl=10; const char *f=0;
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'&&argv[i][1]=='n'&&i+1<argc){nl=ai(argv[++i]);}
        else f=argv[i];
    }
    if(!f){print("usage: head [-n N] file\n");return 1;}
    int n=readfile(f,buf,sizeof(buf));
    if(n<0){print("head: no such file\n");return 1;}
    int L=0;
    for(int i=0;i<n&&L<nl;i++){putchar_(buf[i]); if(buf[i]=='\n')L++;}
    return 0;
}
