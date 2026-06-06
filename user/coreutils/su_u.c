#include "../trinux.h"
int main(int argc, char **argv){
    const char *u = argc>=2 ? argv[1] : "root";
    char pass[64]; int pl=0;
    /* Pedir password siempre, incluso si eres root.
     * Root puede usar password vacío (Enter) para entrar a cualquier user. */
    print("Password for ");
    print(u);
    print(" (Enter if root): ");
    for(;;){
        int k=key_raw();
        if(k=='\n'){putchar_('\n'); pass[pl]=0; break;}
        if(k=='\b'){if(pl>0){pl--; putchar_('\b'); putchar_(' '); putchar_('\b');}}
        else if(k>=32&&k<127){if(pl<63){pass[pl++]=(char)k; putchar_('*');}}
    }
    if(su_(u, pass)<0){
        print("su: authentication failure\n");
        return 1;
    }
    print("Switched to "); print(u);
    print(". (Logout to revert.)\n");
    return 0;
}
