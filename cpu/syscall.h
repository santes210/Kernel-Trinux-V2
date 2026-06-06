#ifndef CPU_SYSCALL_H
#define CPU_SYSCALL_H

#include "idt.h"

/* Syscall numbers: la fuente única de verdad vive en user/trinux.h
 * (compartida con los programas ELF de /bin). El kernel los importa
 * con TRINUX_KERNEL_INCLUDE para no arrastrar los typedef de uint32_t
 * (ya los tiene en lib/types.h) ni las inline wrappers. */
#define TRINUX_KERNEL_INCLUDE 1
#include "../user/trinux.h"

/* Instala el gate int 0x80 con DPL=3 (ring-3 puede invocarlo). */
void syscall_install(void);

/* Si hay un programa ring-3 activo, mátalo en vez de panic. */
bool usermode_fault_kill(int signal_code);

/* Despachador en C invocado desde syscall_asm.asm. */
void syscall_handler(registers_t *regs);

/* Arranca un programa ring-3 plain (sin ELF) y lo registra como proceso. */
int  usermode_run(const char *name, void (*entry)(void));

#endif /* CPU_SYSCALL_H */
