#include "../trinux.h"
static char buf[16384];
int main(int argc, char **argv){
    if(argc<3){print("usage: mv <src> <dst>\n");return 1;}
    int n = readfile(argv[1], buf, sizeof(buf));
    if(n<0){print("mv: "); print(argv[1]); print(": no such file\n");return 1;}
    /* Igual que cp: borrar destino si existe antes de escribir. */
    unlink(argv[2]);
    int w = writefile(argv[2], buf, n);
    if(w<0){print("mv: cannot write\n");return 1;}
    if(w!=n){print("mv: short write\n");return 1;}
    unlink(argv[1]);
    return 0;
}
