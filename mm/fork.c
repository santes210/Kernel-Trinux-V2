/* mm/fork.c — fork() REAL con address space independiente (v0.6.0).
 *
 * Piezas:
 *   1. vmm_create_address_space() crea el PD del hijo (tablas de usuario
 *      32-63 PRIVADAS y vacías; kernel/physmap/MMIO compartidos).
 *   2. vmm_share_user_space() marca cada página presente del padre
 *      como RO+COW en padre E hijo (mismos frames, refcount en el PMM);
 *      la primera escritura copia bajo demanda — Copy-on-Write (v0.6.1).
 *   3. Se copia el trap frame COMPLETO del int 0x80 en curso a la cima
 *      del kstack del hijo, con eax=0 (el hijo "regresa 0" de fork()).
 *   4. El contexto del hijo apunta a fork_child_trampoline (asm), que
 *      imita la cola de syscall_stub (pop ds / popa / add esp,8 / iret):
 *      cuando el scheduler elija al hijo, el iret lo devuelve a ring 3
 *      exactamente después de su instrucción `int 0x80`.
 *   5. El padre sigue en su syscall y fork() le devuelve el PID del hijo.
 *
 * El modelo de ejecución de Trinux es cooperativo-asíncrono: padre e hijo
 * se alternan cuando alguno llama a una syscall que cede la CPU
 * (waitpid/yield/sleep...).  La salida del hijo no usa el setjmp/longjmp
 * del ELF loader (eso es del spawn síncrono): process_exit() marca al
 * hijo ZOMBIE y el flujo regresa directamente al contexto del padre
 * (ver cpu/syscall.c — rama sin jump buffer armado para el pid actual).
 */
#include "fork.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../process/scheduler.h"
#include "../drivers/serial.h"

/* En cpu/syscall_asm.asm — cola de retorno a ring 3 del hijo de fork(). */
extern void fork_child_trampoline(void);
extern void scheduler_add(process_t *proc);

/* Libera los frames físicos del address space de usuario de un proceso. */
void process_free_address_space(process_t *p)
{
    if (!p || !p->page_dir) return;
    vmm_free_address_space(p->page_dir);
    p->page_dir = 0;
}

int process_fork(registers_t *regs)
{
    process_t *parent = process_get_current();
    if (!parent || !regs) return -1;
    if (!parent->page_dir) {
        /* Sólo procesos con address space pueden forkear (kthreads del
         * kernel no — no tienen región de usuario que copiar). */
        kprintf("[fork] el proceso actual no tiene address space\n");
        return -1;
    }

    /* 1. PD del hijo (tablas de usuario privadas vacías). */
    uint32_t child_pd = vmm_create_address_space();
    if (!child_pd) {
        kprintf("[fork] sin frames para el page directory del hijo\n");
        return -1;
    }

    /* 2. Compartir COW la región de usuario del padre. */
    if (vmm_share_user_space(child_pd, parent->page_dir) != 0) {
        vmm_free_address_space(child_pd);
        kprintf("[fork] sin frames para compartir el address space\n");
        return -1;
    }

    /* 3. Registrar el proceso hijo.  process_create() ya le crea un PD
     * propio (vacío): lo sustituimos por el que acabamos de llenar. */
    process_t *child = process_create(parent->name, NULL);
    if (!child) {
        vmm_free_address_space(child_pd);
        kprintf("[fork] process_create falló\n");
        return -1;
    }
    if (child->page_dir)
        vmm_free_address_space(child->page_dir);
    child->page_dir = child_pd;

    /* Herencia POSIX-básica: cwd, límites del heap, handlers de señal,
     * prioridad base.  parent_pid ya lo puso process_create(). */
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);
    child->heap_start = parent->heap_start;
    child->heap_brk   = parent->heap_brk;
    for (int s = 0; s < _NSIG; s++)
        child->sig_handlers[s] = parent->sig_handlers[s];
    child->priority = parent->priority;

    /* 4. Copia del trap frame a la cima del kstack del hijo.
     *
     * LA CUMBRE del kstack la usa context_switch() (ver process/switch.asm):
     * escribe la EIP nueva en [esp] antes de hacer `ret`, así que el
     * contexto debe arrancar 4 bytes POR DEBAJO del frame real para que
     * `ret` consuma ese slot y el esp quede apuntando al frame. */
    uint32_t ktop = (uint32_t)child->kstack + 8192;
    registers_t *cf = (registers_t *)(ktop - sizeof(registers_t));
    memcpy(cf, regs, sizeof(registers_t));
    cf->eax = 0;   /* fork() devuelve 0 en el hijo */

    child->context.eip    = (uint32_t)fork_child_trampoline;
    child->context.esp    = (uint32_t)cf - 4;
    child->context.ebp    = 0;
    child->context.ebx    = 0;
    child->context.esi    = 0;
    child->context.edi    = 0;
    child->context.eflags = 0x202;            /* IF habilitado */
    child->state  = PROC_READY;

    serial_printf("[fork] hijo pid=%u listo (pd=%08x, esp_tramp=%08x)\n",
                  child->pid, child_pd, (uint32_t)cf);

    scheduler_add(child);
    return (int)child->pid;                   /* el padre recibe el PID */
}
