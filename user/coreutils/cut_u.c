#include "../trinux.h"
static char buf[8192];
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    char delim=' '; int field=1; int char_mode=0; int first=1,last=0;
    const char *file=0;
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'&&argv[i][1]=='d'&&i+1<argc){delim=argv[++i][0];}
        else if(argv[i][0]=='-'&&argv[i][1]=='f'&&i+1<argc){field=ai(argv[++i]);}
        else if(argv[i][0]=='-'&&argv[i][1]=='c'&&i+1<argc){
            const char *r=argv[++i]; char_mode=1; first=ai(r); last=first;
            for(int k=0;r[k];k++) if(r[k]=='-'){last=ai(r+k+1);break;}
        } else file=argv[i];
    }
    if(!file){print("usage: cut [-d X -f N | -c A-B] <file>\n");return 1;}
    int n=readfile(file,buf,sizeof(buf)-1);
    if(n<0){print("cut: no such file\n");return 1;}
    buf[n]=0;
    int ls=0;
    for(int i=0;i<=n;i++){
        if(buf[i]=='\n'||buf[i]==0){
            buf[i]=0;
            if(char_mode){
                int len=i-ls; int a=first-1,b=last;
                if(a<0)a=0; if(b>len)b=len;
                for(int k=a;k<b;k++) putchar_(buf[ls+k]);
            } else {
                int cf=1,fs=ls;
                for(int k=ls;k<=i;k++){
                    if(buf[k]==delim||buf[k]==0){
                        if(cf==field){for(int q=fs;q<k;q++) putchar_(buf[q]); break;}
                        cf++; fs=k+1;
                    }
                }
            }
            putchar_('\n');
            ls=i+1;
        }
    }
    return 0;
}
