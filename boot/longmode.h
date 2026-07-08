/* boot/longmode.h — API para detección y entrada a Long Mode (64 bits).
 *
 * CAMBIO #11: soporte de 64 bits para Trinux (preparación de infraestructura).
 *
 * En la versión actual (32-bit), estas funciones se usan solo para:
 *   1. Detectar si la CPU es capaz de 64 bits (longmode_check).
 *   2. Reportarlo en el banner de boot.
 *
 * Para activar el modo 64-bit completo se necesitaría:
 *   - Un linker script separado (-Ttext=0x100000 con ELF64)
 *   - Recompilar el kernel con -m64 y cross-compiler x86_64-elf-gcc
 *   - Reescribir GDT, IDT, syscall (SYSCALL/SYSRET en lugar de int 0x80)
 *   - Adaptar el scheduler y el manejo de procesos para registros de 64 bits
 *
 * Hoja de ruta de 64 bits:
 *   Fase A (este cambio): infraestructura de detección y trampoline
 *   Fase B: recompilar kernel con -m64 y nuevo linker.ld
 *   Fase C: IDT de 64 bits (16-byte entries con IST)
 *   Fase D: syscall vía SYSCALL/SYSRET en lugar de int 0x80
 *   Fase E: paginación de 4 niveles (PML4) para acceso a > 4 GiB
 */
#ifndef BOOT_LONGMODE_H
#define BOOT_LONGMODE_H

#include "../lib/types.h"

/* Verifica via CPUID si la CPU soporta Long Mode (x86_64).
 * Devuelve 1 si soportado, 0 si no. */
int longmode_check(void);

/* Configura las tablas de paginación de 64 bits (PML4/PDPT/PD) en
 * memoria baja (0x1000-0x3FFF), habilita Long Mode en EFER, y salta
 * al entry point de 64 bits especificado.
 * No regresa. Solo se llama desde boot para un kernel de 64 bits. */
void longmode_enter(void (*entry64)(void));

#endif /* BOOT_LONGMODE_H */
