/* execvetest: prueba manual de SYS_EXECVE (execve() real).
 *
 * Primera pasada (sin args): imprime su PID, arma un argv con "child" y
 * llama execve_() sobre sí mismo. Si execve() funciona de verdad, el
 * proceso se reemplaza in-place -- MISMO PID, pero corriendo la rama
 * "child" del programa. Si execve_() falla, cae al print de error (el
 * proceso original sigue vivo, tal como el execve(2) real de Unix).
 *
 * Uso:  exec /bin/execvetest
 * Esperado:
 *   [execvetest] PID antes de execve()=N
 *   [execvetest] Soy el proceso reemplazado (post-execve). PID=N argv[1]=child
 * El PID debe ser IDÉNTICO en ambas líneas -- eso es lo que prueba que
 * execve() reemplazó la imagen en vez de crear un proceso nuevo.
 */
#include "../trinux.h"

int main(int argc, char **argv) {
    if (argc >= 2 && streq(argv[1], "child")) {
        print("[execvetest] Soy el proceso reemplazado (post-execve). PID=");
        print_num(getpid());
        print(" argv[1]=");
        print(argv[1]);
        print("\n");
        print("[execvetest] Si el PID de arriba es igual al de antes, execve() funciono.\n");
        return 42;
    }

    print("[execvetest] PID antes de execve()=");
    print_num(getpid());
    print("\n");

    char *newargv[3];
    newargv[0] = "/bin/execvetest";
    newargv[1] = "child";
    newargv[2] = 0;

    int rc = execve_("/bin/execvetest", newargv);

    /* Si llegamos aqui, execve_() fallo -- el proceso original sigue vivo. */
    print("[execvetest] execve_() FALLO, rc=");
    print_num(rc);
    print(" (deberia haber reemplazado este proceso y no regresar aqui)\n");
    return 1;
}
