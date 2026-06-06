#include "../trinux.h"
static char buf[16384];
int main(int argc, char **argv){
    if(argc<3){print("usage: cp <src> <dst>\n");return 1;}
    int n = readfile(argv[1], buf, sizeof(buf));
    if(n<0){print("cp: "); print(argv[1]); print(": no such file\n");return 1;}
    /* Si el destino existe, lo borramos primero para forzar un node fresh.
     * Sin esto, vfs_create devuelve el nodo existente sin truncar y los
     * datos viejos pueden quedar al final del archivo nuevo. */
    unlink(argv[2]);
    int w = writefile(argv[2], buf, n);
    if(w<0){print("cp: cannot write "); print(argv[2]); print("\n");return 1;}
    if(w!=n){print("cp: short write\n");return 1;}
    return 0;
}
