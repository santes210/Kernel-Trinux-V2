/* boot/gdt.h — Global Descriptor Table y TSS (interfaz pública).
 *
 * Antes estas funciones se declaraban con `extern` sueltos (o sin declarar,
 * lo que provocaba declaraciones implícitas -Wimplicit-function-declaration
 * en kernel/elf.c). Centralizarlas aquí da un único punto de verdad.
 */
#ifndef BOOT_GDT_H
#define BOOT_GDT_H

#include "../lib/types.h"

/* Instala la GDT (segmentos kernel/user + TSS) y carga los selectores. */
void gdt_install(void);

/* Actualiza el puntero de stack de ring 0 (ESP0) de la TSS. El kernel lo
 * invoca antes de cada entrada/salida de ring 3 para que una interrupción
 * ocurrida en userspace use el kernel stack correcto del proceso actual. */
void tss_set_kernel_stack(uint32_t esp0);

#endif /* BOOT_GDT_H */
