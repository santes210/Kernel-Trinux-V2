/* cpu/uaccess.h — Validación de punteros userspace en syscalls.
 *
 * CAMBIO #2 — Seguridad: antes de usar cualquier puntero que llega
 * desde ring 3 (via ebx/ecx/edx en int 0x80), el kernel debe verificar
 * que apunta a la región de usuario y no al espacio del kernel.
 *
 * Rango de memoria de usuario:
 *   0x08048000 (base ELF)  ..  0x0F000000 (USER_STACK_TOP)
 *
 * Un proceso malicioso podría pasar un puntero al kernel (ej. 0x00100000)
 * y hacer que el syscall handler lea/escriba la memoria del kernel.
 * Esta capa lo evita.
 */
#ifndef CPU_UACCESS_H
#define CPU_UACCESS_H

#include "../lib/types.h"

/* Límites del espacio de usuario (coincide con elf.c y syscall.c). */
#define USER_SPACE_START  0x08000000u   /* base mínima razonable de ELF */
#define USER_STACK_TOP    0x0F000000u   /* tope del stack de usuario */
#define USER_SPACE_END    0x10000000u   /* fin del identity-map de 256 MiB */

/* Devuelve true si el rango [ptr, ptr+len) está completamente dentro
 * del espacio de usuario (no puede tocar el kernel ni el hardware). */
static inline bool uaccess_ok(const void *ptr, uint32_t len)
{
    if (!ptr)                          return false;
    uint32_t addr = (uint32_t)ptr;
    /* Verificar que el rango no se desborde y esté en zona de usuario. */
    if (addr < USER_SPACE_START)       return false;
    if (addr >= USER_SPACE_END)        return false;
    if (len > 0 && (addr + len) > USER_SPACE_END) return false;
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
