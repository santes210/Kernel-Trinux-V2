#include "../trinux.h"
static char out[64][256];
int main(int argc, char **argv){
    find_req_t r;
    r.root_path = argc>=2?argv[1]:"/";
    r.name_substr = (argc>=4 && streq(argv[2],"-name")) ? argv[3] : 0;
    r.max = 64; r.out_paths = out;
    int n = find_(&r);
    for(int i=0;i<n;i++) println(out[i]);
    return 0;
}
