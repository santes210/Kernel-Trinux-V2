#include "../trinux.h"
int main(int argc, char **argv){
    tree_req_t r; r.root_path = argc>=2?argv[1]:"."; r.max_depth=8;
    if(tree_(&r)<0){print("tree: error\n");return 1;}
    return 0;
}
