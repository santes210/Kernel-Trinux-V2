#include "../trinux.h"
int main(int argc, char **argv){
    char user[64];
    if(argc>=2){int i=0; while(argv[1][i]&&i<63){user[i]=argv[1][i];i++;} user[i]=0;}
    else getuser(user, sizeof(user));
    char p1[64],p2[64]; int pl;
    print("New password: "); pl=0;
    for(;;){int k=key_raw(); if(k=='\n'){putchar_('\n');p1[pl]=0;break;}
        if(k=='\b'){if(pl>0){pl--;putchar_('\b');putchar_(' ');putchar_('\b');}}
        else if(k>=32&&k<127){if(pl<63){p1[pl++]=(char)k;putchar_('*');}}}
    print("Repeat   : "); pl=0;
    for(;;){int k=key_raw(); if(k=='\n'){putchar_('\n');p2[pl]=0;break;}
        if(k=='\b'){if(pl>0){pl--;putchar_('\b');putchar_(' ');putchar_('\b');}}
        else if(k>=32&&k<127){if(pl<63){p2[pl++]=(char)k;putchar_('*');}}}
    if(!streq(p1,p2)){print("passwd: don't match\n");return 1;}
    passwd_req_t r; r.user=user; r.new_pass=p1;
    if(passwd_(&r)<0){print("passwd: failed\n");return 1;}
    print("password updated\n");
    return 0;
}
