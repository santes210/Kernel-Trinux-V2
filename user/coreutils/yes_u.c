#include "../trinux.h"
int main(int argc,char**argv){
    const char *s=(argc>=2)?argv[1]:"y";
    for(int i=0;i<200;i++){print(s);print("\n");}
    return 0;
}
