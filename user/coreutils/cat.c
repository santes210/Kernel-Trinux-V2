#include "../trinux.h"
static char buf[8192];
int main(int argc, char **argv) {
    if (argc < 2) { print("usage: cat <file>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int n = readfile(argv[i], buf, sizeof(buf));
        if (n < 0) { print("cat: "); print(argv[i]); print(": no such file\n"); rc=1; continue; }
        write(1, buf, n);
    }
    return rc;
}
