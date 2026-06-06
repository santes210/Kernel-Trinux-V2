#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: stat <file>\n");return 1;}
    trinux_stat_t st;
    if(stat(argv[1], &st)<0){print("stat: no such file\n");return 1;}
    print("  File: "); print(argv[1]); print("\n  Type: ");
    if(st.type==1) print("regular file"); else if(st.type==2) print("directory");
    else if(st.type==3) print("device"); else print("?");
    print("\n  Size: "); print_unum(st.size);
    print("\n  Perms: 0"); print_unum(st.perm);
    print("\n  Uid: "); print_unum(st.uid); print("   Gid: "); print_unum(st.gid); print("\n");
    return 0;
}
