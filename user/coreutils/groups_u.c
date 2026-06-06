#include "../trinux.h"
static struct {uint32_t uid,gid;} gl[8];
int main(int argc, char **argv){(void)argc;(void)argv;
    groups_req_t r; r.list=(void*)gl; r.max=8;
    int n=getgroups_(&r);
    for(int i=0;i<n;i++){print_unum(gl[i].gid); print(" ");} print("\n");
    return 0;
}
