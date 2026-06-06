/* cpu/smp.h — Symmetric Multi-Processing (Fase 1: solo detección).
 *
 * Esta fase SOLO detecta cuántos cores tiene el procesador, parseando
 * la tabla ACPI MADT (Multiple APIC Description Table). NO arranca
 * los Application Processors (APs) todavía — eso es Fase 2.
 *
 * Si SMP falla por cualquier razón (no hay ACPI, no hay LAPIC, no hay
 * MADT, etc.) el kernel sigue funcionando en single-core como siempre.
 */
#ifndef CPU_SMP_H
#define CPU_SMP_H

#include "../lib/types.h"

#define SMP_MAX_CPUS 8

typedef struct {
    uint8_t  apic_id;        /* identificador del LAPIC */
    uint8_t  acpi_id;        /* processor UID ACPI */
    bool     enabled;        /* core utilizable */
    bool     online;         /* arrancado (siempre true para BSP) */
} smp_cpu_t;

/* Detecta cores via ACPI MADT. Devuelve cuántos encontró. Si no encuentra
 * ACPI o falla por cualquier razón, devuelve 1 (solo el BSP). */
int  smp_detect(void);

/* Acceso a la lista de cores detectados. */
int         smp_cpu_count(void);
smp_cpu_t  *smp_cpu_at(int i);

/* Direccion fisica del LAPIC (típicamente 0xFEE00000). 0 si no se detectó. */
uint32_t    smp_lapic_base(void);

/* APIC ID del BSP (Bootstrap Processor — el core que corrió el boot). */
uint8_t     smp_bsp_apic_id(void);

#endif /* CPU_SMP_H */
