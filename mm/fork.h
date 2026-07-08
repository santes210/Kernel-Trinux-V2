/* mm/fork.h — fork() real para Trinux.
 *
 * CAMBIO #4: fork() con address space independiente por proceso.
 *
 * Implementa la creación de un proceso hijo que hereda una copia exacta
 * del espacio de direcciones del padre. Usa vmm_create_address_space()
 * (ya existente en vmm.c) y copia física de las páginas del padre.
 *
 * Limitaciones intencionales para esta versión educativa:
 *   - No hay Copy-on-Write (COW): se copia toda la región del ELF
 *     (0x08048000..0x0F000000) físicamente. COW requeriría trampas en
 *     page fault que complicarían el modelo de un solo address space.
 *   - El hijo hereda el estado del kernel (kheap, VFS) que es read-only
 *     en la práctica — el mismo que en el sistema anterior de snapshot.
 *   - Pensado para procesos de corta vida (comandos) que inmediatamente
 *     hacen exec o exit, no para procesos de larga duración que muten
 *     grandes cantidades de memoria compartida.
 */
#ifndef MM_FORK_H
#define MM_FORK_H

#include "../lib/types.h"
#include "../process/process.h"

/* Crea un hijo del proceso actual.
 * - Duplica el address space del padre (región ELF 0x08048000..0x0F000000).
 * - Crea un nuevo process_t con el mismo contexto.
 * - Devuelve el PID del hijo al padre, 0 al hijo.
 * - Devuelve -1 si no hay recursos.
 *
 * NOTA: en Trinux, como la shell corre en el flujo del kernel (no en un
 * proceso real schedulable independiente), fork() se implementa como un
 * "fork-exec" implícito: el hijo se ejecuta hasta SYS_EXIT y luego vuelve
 * al padre. Semánticamente equivalente a posix_spawn() pero con address
 * space copiado. */
int process_fork(void);

/* Libera el address space de un proceso hijo tras su terminación. */
void process_free_address_space(process_t *p);

/* Copia física las páginas de la región [start..end) del espacio actual
 * al nuevo page directory `child_pd`. Usado internamente por fork(). */
void vmm_copy_region(uint32_t child_pd, uint32_t start, uint32_t end);

#endif /* MM_FORK_H */
