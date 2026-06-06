#include "../trinux.h"
static struct {uint32_t pid; char name[32]; uint32_t cpu_ticks; uint32_t state; int priority;} list[64];
static const char *sn(uint32_t s){switch(s){case 0:return"RUN";case 1:return"RDY";case 2:return"SLP";case 3:return"ZMB";} return "?";}
int main(int argc, char **argv){(void)argc;(void)argv;
    plist_req_t r; r.list=(void*)list; r.max=64;
    int n=listproc_(&r);
    print("  PID  ST   PRI  TICKS  NAME\n");
    for(int i=0;i<n;i++){
        if(list[i].pid<100) putchar_(' ');
        if(list[i].pid<10) putchar_(' ');
        print_unum(list[i].pid); print("   ");
        print(sn(list[i].state)); print("  ");
        print_num(list[i].priority); print("    ");
        print_unum(list[i].cpu_ticks); print("    ");
        print(list[i].name); print("\n");
    }
    return 0;
}
