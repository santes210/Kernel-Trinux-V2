#include "../trinux.h"
#define ML 256
static char buf[16384];
static char *lines[ML];
static int slt(const char*a,const char*b){while(*a&&*a==*b){a++;b++;} return (uint8_t)*a-(uint8_t)*b;}
int main(int argc, char **argv){
    int rev=0,num=0,uniq=0; const char *file=0;
    for(int i=1;i<argc;i++){
        if(streq(argv[i],"-r"))rev=1;
        else if(streq(argv[i],"-n"))num=1;
        else if(streq(argv[i],"-u"))uniq=1;
        else file=argv[i];
    }
    if(!file){print("usage: sort [-rnu] <file>\n");return 1;}
    int n=readfile(file,buf,sizeof(buf)-1);
    if(n<0){print("sort: no such file\n");return 1;}
    buf[n]=0;
    int nl=0; lines[nl++]=buf;
    for(int i=0;i<n && nl<ML;i++) if(buf[i]=='\n'){buf[i]=0; if(i+1<n) lines[nl++]=&buf[i+1];}
    for(int i=0;i<nl-1;i++) for(int j=0;j<nl-1-i;j++){
        int cmp;
        if(num){int a=0,b=0; const char *pa=lines[j],*pb=lines[j+1];
            while(*pa>='0'&&*pa<='9'){a=a*10+*pa-'0';pa++;}
            while(*pb>='0'&&*pb<='9'){b=b*10+*pb-'0';pb++;}
            cmp = a-b;
        } else cmp = slt(lines[j], lines[j+1]);
        if((!rev&&cmp>0)||(rev&&cmp<0)){char *t=lines[j]; lines[j]=lines[j+1]; lines[j+1]=t;}
    }
    const char *prev=0;
    for(int i=0;i<nl;i++){
        if(uniq&&prev&&slt(prev,lines[i])==0) continue;
        print(lines[i]); print("\n"); prev=lines[i];
    }
    return 0;
}
