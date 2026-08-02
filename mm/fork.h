/* mm/fork.h — fork() real para Trinux (v0.6.0).
 *
 * fork() con address space independiente: el hijo recibe una deep-copy
 * de la región de usuario privada del padre (tablas 32-63) dentro de su
 * propio page directory, y una copia del trap frame del int 0x80 en
 * curso con eax=0 — de modo que fork() regresa DOS veces: en el padre
 * devuelve el PID del hijo, en el hijo devuelve 0.
 *
 * Limitaciones intencionales (v1):
 *   - El modelo de scheduling es cooperativo: padre e hijo se turnan en
 *     los puntos de syscall (waitpid/yield/sleep), como el resto de
 *     Trinux. fork() desde procesos que nunca hagan syscalls tras el
 *     fork no progresa el otro lado (igual que antes).
 *   - La fd-table sigue siendo global: el hijo "hereda" los fds del
 *     padre en el sentido de que la tabla es compartida por diseño.
 */
#ifndef MM_FORK_H
#define MM_FORK_H

#include "../lib/types.h"
#include "../process/process.h"
#include "../cpu/idt.h"        /* registers_t (trap frame int 0x80) */

/* Crea un hijo del proceso actual.
 *
 *  `regs` = trap frame del int 0x80 en curso (el hijo lo hereda con
 *           eax=0 para que su fork() "regrese 0").
 *
 * Devuelve en el PADRE el PID del hijo (>0); en el HIJO, 0; -1 error.
 */
int process_fork(registers_t *regs);

/* Libera el address space de un proceso (vmm_free_address_space). */
void process_free_address_space(process_t *p);

#endif /* MM_FORK_H */
