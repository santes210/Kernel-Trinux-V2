#include "../trinux.h"
static void pp2(int n){if(n<10) putchar_('0'); print_num(n);}
int main(int argc, char **argv){(void)argc;(void)argv;
    datetime_u_t t; if(datetime_(&t)<0){print("date: error\n");return 1;}
    print_unum(t.year); print("-"); pp2(t.month); print("-"); pp2(t.day);
    print(" "); pp2(t.hour); print(":"); pp2(t.minute); print(":"); pp2(t.second); print("\n");
    return 0;
}
