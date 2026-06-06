#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: basename <path>\n");return 1;}
    const char *s=argv[1]; const char *last=s;
    for(const char *p=s;*p;p++) if(*p=='/') last=p+1;
    println(last);
    return 0;
}
