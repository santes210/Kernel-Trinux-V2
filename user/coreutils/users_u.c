#include "../trinux.h"
static struct {char name[32]; uint32_t uid,gid; char home[64];} list[16];
int main(int argc, char **argv){(void)argc;(void)argv;
    user_list_req_t r; r.list=(void*)list; r.max=16;
    int n=userlist_(&r);
    print("USER         UID   GID   HOME\n");
    for(int i=0;i<n;i++){
        print(list[i].name);
        int pad=13-strlen_(list[i].name); if(pad<1)pad=1;
        for(int k=0;k<pad;k++) putchar_(' ');
        print_unum(list[i].uid); print("    "); print_unum(list[i].gid); print("    ");
        print(list[i].home); print("\n");
    }
    return 0;
}
