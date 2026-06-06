#include "../trinux.h"
static char buf[16384];
static int slt(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return(uint8_t)*a-(uint8_t)*b;}
int main(int argc, char **argv){
    int cflag=0; const char *file=0;
    for(int i=1;i<argc;i++){if(streq(argv[i],"-c"))cflag=1; else file=argv[i];}
    if(!file){print("usage: uniq [-c] <file>\n");return 1;}
    int n=readfile(file,buf,sizeof(buf)-1);
    if(n<0){print("uniq: no such file\n");return 1;}
    buf[n]=0;
    char *p=buf,*prev=0; int cnt=1;
    while(p<buf+n){
        char *eol=p; while(*eol&&*eol!='\n') eol++;
        char saved=*eol; *eol=0;
        if(prev&&slt(prev,p)==0) cnt++;
        else {if(prev){if(cflag){print_num(cnt);print(" ");} print(prev); print("\n");} prev=p; cnt=1;}
        *eol=saved;
        p = (*eol) ? eol+1 : eol;
    }
    if(prev){if(cflag){print_num(cnt);print(" ");} print(prev); print("\n");}
    return 0;
}
