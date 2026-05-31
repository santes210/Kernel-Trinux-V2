#ifndef CPU_SYSCALL_H
#define CPU_SYSCALL_H

#include "idt.h"

/* Syscall numbers (passed in eax). Mirrors a tiny subset of Linux's i386 ABI
 * spirit, not its exact numbers. Args: ebx, ecx, edx. Return value: eax. */
#define SYS_EXIT    1   /* ebx = exit code; never returns to the user        */
#define SYS_WRITE   2   /* ebx = fd(ignored), ecx = buf, edx = len -> bytes  */
#define SYS_GETPID  3   /* -> current pid                                    */
#define SYS_YIELD   4   /* cooperatively give up the CPU (calls schedule())  */
#define SYS_SLEEP   5   /* ebx = milliseconds                                */
#define SYS_GETC    6   /* blocking read of one key -> char                  */
#define SYS_UPTIME  7   /* -> seconds since boot                             */

/* Install the int 0x80 gate (DPL 3 so ring-3 code may invoke it). */
void syscall_install(void);

/* If a ring-3 program is running, terminate it (return to the kernel) instead
 * of panicking. Used by the CPU exception handler. Returns false if no user
 * program is active (so the kernel should panic as usual). */
bool usermode_fault_kill(int signal_code);

/* C dispatcher called from syscall_asm.asm. */
void syscall_handler(registers_t *regs);

/* Run a ring-3 program: switches to user mode at `entry` with a fresh user
 * stack, and returns here when the program calls SYS_EXIT. Returns the exit
 * code. `name` is used for the process table (ps/top). */
int  usermode_run(const char *name, void (*entry)(void));

#endif /* CPU_SYSCALL_H */
