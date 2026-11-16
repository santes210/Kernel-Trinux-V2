/* cpu/uaccess.h — Validación de punteros userspace en syscalls.
 *
 * CAMBIO #2 — Seguridad: antes de usar cualquier puntero que llega
 * desde ring 3 (via ebx/ecx/edx en int 0x80), el kernel debe verificar
 * que apunta a la región de usuario y no al espacio del kernel.
 *
 * Rango de memoria de usuario (ver include/userlayout.h, fuente única):
 *   0x05000000 (frontera kernel)  ..  0x10000000 (USER_SPACE_END)
 *
 * Un proceso malicioso podría pasar un puntero al kernel (ej. 0x00100000)
 * y hacer que el syscall handler lea/escriba la memoria del kernel.
 * Esta capa lo evita.
 */
#ifndef CPU_UACCESS_H
#define CPU_UACCESS_H

#include "../lib/types.h"
#include "../include/userlayout.h"

/* Los límites viven en include/userlayout.h para que vmm.c, fork.c,
 * elf.c y syscall.c no puedan volver a desincronizarse. */
#define USER_STACK_TOP    USER_STACK_TOP_L0

/* Devuelve true si el rango [ptr, ptr+len) está completamente dentro
 * del espacio de usuario (no puede tocar el kernel ni el hardware). */
static inline bool uaccess_ok(const void *ptr, uint32_t len)
{
    if (!ptr)                          return false;
    uint32_t addr = (uint32_t)ptr;
    /* Verificar que el rango no se desborde y esté en zona de usuario.
     *
     * OJO con el orden: `addr + len` puede desbordar a 32 bits (addr alto +
     * len grande) y dar un valor pequeño que pasa el test. Por eso se compara
     * contra la distancia restante, no contra la suma. */
    if (addr < USER_SPACE_START)       return false;
    if (addr >= USER_SPACE_END)        return false;
    if (len > (USER_SPACE_END - addr)) return false;
    return true;
}

/* Igual, para strings: verifica que el inicio esté en zona de usuario
 * y que haya un '\0' dentro del límite de USER_SPACE_END.
 * Devuelve la longitud del string o -1 si es inválido. */
static inline int32_t uaccess_strnlen(const char *ptr, uint32_t maxlen)
{
    if (!ptr)                          return -1;
    uint32_t addr = (uint32_t)ptr;
    if (addr < USER_SPACE_START)       return -1;
    if (addr >= USER_SPACE_END)        return -1;

    uint32_t limit = maxlen;
    uint32_t avail = USER_SPACE_END - addr;
    if (limit > avail) limit = avail;

    for (uint32_t i = 0; i < limit; i++)
        if (ptr[i] == '\0')
            return (int32_t)i;

    return -1;  /* sin terminador dentro del límite */
}

/* Macro de conveniencia: si el puntero no es válido, pone -EFAULT en eax
 * y hace break del switch del syscall handler. */
#define UCHECK_PTR(ptr, len) \
    do { if (!uaccess_ok((ptr), (len))) { regs->eax = (uint32_t)-14; break; } } while(0)

#define UCHECK_STR(ptr) \
    do { if (uaccess_strnlen((const char *)(ptr), 4096) < 0) \
             { regs->eax = (uint32_t)-14; break; } } while(0)

#endif /* CPU_UACCESS_H */
