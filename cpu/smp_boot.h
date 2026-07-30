/* cpu/smp_boot.h — Arranque de Application Processors (APs) via LAPIC IPI.
 *
 * CAMBIO #6: arranca los cores adicionales detectados por smp_detect().
 *
 * Protocolo Intel MP (Multi-Processor):
 *   1. El BSP (Bootstrap Processor) escribe un trampoline code en memoria
 *      baja (< 1 MiB) en una dirección alineada a 4 KiB.
 *   2. El BSP envía INIT IPI al AP (lo resetea).
 *   3. El BSP envía dos STARTUP IPI al AP con la dirección del trampoline
 *      (en unidades de 4 KiB), para que el AP arranque en real mode en ese vector.
 *   4. El trampoline en real mode habilita protected mode y salta al AP entry.
 *   5. El AP entry inicializa su GDT, paginación, y llama ap_main().
 *   6. ap_main() registra el AP en el scheduler y empieza a ejecutar tareas.
 *
 * Estado actual: Fase 1 del SMP (detección) → Fase 2 (boot de APs).
 * El trampoline está embebido como bytes x86 en este header para no
 * necesitar un archivo ASM extra compilado por separado.
 */
#ifndef CPU_SMP_BOOT_H
#define CPU_SMP_BOOT_H

#include "../lib/types.h"
#include "smp.h"

/* Dirección del trampoline en memoria baja (debe estar < 1 MiB, alineada 4K).
 * Usamos 0x8000 (32 KiB) — libre en QEMU y en la mayoría de hardware real. */
#define AP_TRAMPOLINE_ADDR  0x8000

/* Número máximo de APs que este arranque soporta. */
#define SMP_MAX_APS  (SMP_MAX_CPUS - 1)

/* Stack size para cada AP (16 KiB). */
#define AP_STACK_SIZE  (16 * 1024)

/* Arranca todos los APs detectados por smp_detect().
 * Devuelve el número de APs que arrancaron exitosamente (0 si solo hay BSP). */
int smp_boot_aps(void);

/* Callback que llama cada AP tras inicializarse.
 * Registra el AP en el scheduler y entra al loop de ejecución de tareas. */
void ap_main(void);   /* el AP lee su APIC ID del LAPIC (sin args cdecl) */

/* Devuelve cuántos APs están corriendo (además del BSP). */
int smp_aps_online(void);

#endif /* CPU_SMP_BOOT_H */
