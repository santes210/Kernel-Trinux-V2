#include "../trinux.h"
int main(int argc, char **argv) {
    int n_flag = 0, i = 1;
    if (i < argc && argv[i][0]=='-' && argv[i][1]=='n' && argv[i][2]==0) { n_flag=1; i++; }
    for (; i < argc; i++) { print(argv[i]); if (i+1<argc) print(" "); }
    if (!n_flag) print("\n");
    return 0;
}
