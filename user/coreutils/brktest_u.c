/* brktest: prueba manual de SYS_BRK (heap userland real por proceso).
 *
 * 1. brk_(0)             -> consulta el break inicial (justo tras el BSS).
 * 2. brk_(inicial+8192)  -> pide crecer 8 KiB; debe devolver el break
 *                           ANTERIOR (semántica brk() clásica).
 * 3. Escribe y lee un patrón en esas páginas nuevas para confirmar que
 *    de verdad están mapeadas (si no lo estuvieran, esto causaría un
 *    PAGE FAULT y el kernel mataría el proceso con SIGSEGV -- fallo
 *    visible e inequívoco).
 * 4. brk_(inicial-4096)  -> intento de bajar del heap_start: debe
 *                           rechazarse (retorna 0) sin tocar el break.
 *
 * Uso: exec /bin/brktest
 */
#include "../trinux.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    uint32_t initial = (uint32_t)brk_((void *)0);
    print("[brktest] break inicial=");
    print_num((int32_t)initial);
    print("\n");

    uint32_t want = initial + 8192;
    uint32_t old = (uint32_t)brk_((void *)want);
    if (old == 0) {
        print("[brktest] FALLO: brk_(crecer) devolvio 0\n");
        return 1;
    }
    print("[brktest] brk_(crecer) OK, break anterior=");
    print_num((int32_t)old);
    print("\n");

    /* Escribir/leer el heap nuevo para probar que las paginas estan
     * de verdad mapeadas y con permiso de escritura. */
    volatile unsigned char *heap = (volatile unsigned char *)initial;
    for (int i = 0; i < 8192; i++) heap[i] = (unsigned char)(i & 0xFF);
    int ok = 1;
    for (int i = 0; i < 8192; i++) {
        if (heap[i] != (unsigned char)(i & 0xFF)) { ok = 0; break; }
    }
    if (ok) print("[brktest] lectura/escritura del heap nuevo: OK\n");
    else    print("[brktest] lectura/escritura del heap nuevo: FALLO\n");

    /* Intentar encoger por debajo de heap_start -- debe fallar. */
    uint32_t bad = (uint32_t)brk_((void *)(initial - 4096));
    if (bad == 0) {
        print("[brktest] proteccion heap_start: OK (rechazado correctamente)\n");
    } else {
        print("[brktest] proteccion heap_start: FALLO (se acepto un brk invalido)\n");
        return 1;
    }

    print("[brktest] Todas las pruebas pasaron.\n");
    return 0;
}
