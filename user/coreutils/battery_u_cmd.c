#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    battery_u_t b;
    if(battery_(&b)<0||!b.present){print("battery: no battery (VM/AC)\n");return 1;}
    print("battery: "); print_num(b.percent); print("%  (");
    print(b.discharging?"discharging":"charging"); print(")\n");
    return 0;
}
