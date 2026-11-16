/* include/userlayout.h — Mapa de memoria de usuario (ÚNICA fuente de verdad).
 *
 * Antes de v0.6 cada subsistema tenía sus propios números y no cuadraban:
 *   cpu/uaccess.h   decía  0x08000000 .. 0x10000000
 *   mm/vmm.c        decía  usuario desde 0x05000000 (80 MiB)
 *   mm/fork.c       copiaba 0x08048000 .. 0x0F100000 (113 MiB por fork)
 *   cpu/syscall.c   limitaba brk a 0x0E000000
 *   kernel/elf.c    ponía los stacks en 0x0F000000 / 0x0EE00000 / ...
 * Resultado: `brk` podía devolver memoria que se solapaba con el stack del
 * shell y fork() copiaba páginas que ningún proceso usaba.
 *
 * Distribución canónica (identity-mapped, phys == virt):
 *
 *   0x00000000 - 0x05000000  kernel (código, datos, heap, VGA, frames) [U/S=0]
 *   0x05000000 - 0x08048000  hueco reservado para el kernel futuro
 *   0x08048000 - 0x0E000000  ELF: .text/.data/.bss + heap (brk)        [U/S=1]
 *   0x0E000000 - 0x0E800000  hueco (guard) entre heap y stacks
 *   0x0E800000 - 0x0F000000  stacks de usuario (4 niveles x 64 KiB)    [U/S=1]
 *   0x0F000000 - 0x10000000  reservado (no mapeado en procesos hijos)
 *   0x10000000 - ...         libre para allocaciones dinámicas
 */
#ifndef INCLUDE_USERLAYOUT_H
#define INCLUDE_USERLAYOUT_H

/* Frontera kernel / usuario.  Todo lo que hay debajo es supervisor-only. */
#define USER_SPACE_START     0x05000000u

/* Base canónica de los ELF (la que usa elf.c y el linker de userland). */
#define USER_ELF_BASE        0x08048000u

/* Techo del heap de brk(): por debajo de los stacks. */
#define USER_BRK_MAX         0x0E000000u

/* Stacks de usuario: un nivel de anidamiento por cada 0x200000. */
#define USER_STACK_SIZE      0x00010000u   /* 64 KiB por nivel  */
#define USER_STACK_TOP_L0    0x0F000000u   /* shell             */
#define USER_STACK_TOP_L1    0x0EE00000u
#define USER_STACK_TOP_L2    0x0EC00000u
#define USER_STACK_TOP_L3    0x0EA00000u
#define USER_STACK_TOP_FALLBACK 0x0E800000u

/* Tope del espacio de usuario direccionable (fin de lo que valida uaccess). */
#define USER_SPACE_END       0x10000000u

/* Rango que hay que copiar en fork() — código + datos + heap + stacks. */
#define USER_COPY_START      USER_ELF_BASE
#define USER_COPY_END        0x0F000000u

#endif /* INCLUDE_USERLAYOUT_H */
