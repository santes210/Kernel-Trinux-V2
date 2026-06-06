/* ringtest: prueba que efectivamente estamos en ring 3.
 * Si lo estamos, `cli` debe causar #GP y matar al programa.
 * Si NO lo estamos (estamos en ring 0), la línea siguiente se imprime. */
#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v;
    print("[ringtest] Soy un programa userland. PID="); print_num(getpid()); print("\n");
    print("[ringtest] Intentando ejecutar 'cli' (instrucción privilegiada)...\n");
    print("[ringtest] Si ves 'isolation FAILED', estoy en ring 0.\n");
    print("[ringtest] Si en vez de eso el kernel me mata con #GP, estoy en ring 3.\n");
    __asm__ volatile("cli");
    print("[ringtest] ISOLATION FAILED — esto se imprimio aunque hice 'cli'.\n");
    return 99;
}
