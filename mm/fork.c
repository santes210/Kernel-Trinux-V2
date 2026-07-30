/* mm/fork.c — fork() real con copia de address space.
 *
 * CAMBIO #4: cada proceso hijo recibe su propia copia física de la región
 * de usuario, completamente independiente del padre.
 *
 * Flujo de fork():
 *   1. pmm_alloc_frame() × N para obtener frames físicos para las páginas del hijo.
 *   2. vmm_create_address_space() clona el kernel identity-map (256 MiB).
 *   3. vmm_copy_region() itera las páginas de usuario, copia físicamente su
 *      contenido y las mapea en el nuevo page directory.
 *   4. process_create() registra el proceso hijo con su propio page_dir.
 *   5. El kernel ejecuta el hijo y restaura el page directory del padre al volver.
 */
#include "fork.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../process/process.h"
#include "../process/scheduler.h"

/* Rango de memoria de usuario a copiar en fork().
 * Cubre ELF code+data+stack de todos los niveles de spawn. */
#define USER_COPY_START  0x08048000u
#define USER_COPY_END    0x0F100000u   /* un poco por encima del stack top */

#define PAGE_SIZE 4096

/* Copia física las páginas de [start..end) al nuevo page directory.
 * Para cada página alineada en el rango:
 *   1. Alloca un frame físico nuevo.
 *   2. Copia 4 KiB del contenido actual (que está identity-mapped).
 *   3. Mapea el frame en el page directory del hijo con PAGE_USER|PAGE_RW.
 */
void vmm_copy_region(uint32_t child_pd, uint32_t start, uint32_t end)
{
    uint32_t addr = start & ~(PAGE_SIZE - 1);
    while (addr < end) {
        /* Alloca frame físico para el hijo */
        uint32_t new_frame = pmm_alloc_frame();
        if (!new_frame) {
            kprintf("[fork] pmm_alloc_frame failed at %08x\n", addr);
            addr += PAGE_SIZE;
            continue;
        }

        /* Copiar página (el identity-map hace phys==virt, así que podemos
         * leer de `addr` directamente y escribir en `new_frame`). */
        memcpy((void *)new_frame, (void *)addr, PAGE_SIZE);

        /* SECURITY (v0.5.3): retirar el alias identity-mapped del frame del
         * alcance de ring 3 — el hijo accede a su copia SOLO por su VA. */
        vmm_deprivilege_identity_page(new_frame);

        /* Mapear en el page directory del hijo */
        vmm_map_page_in(child_pd, addr, new_frame,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER);

        addr += PAGE_SIZE;
    }
}

/* Libera los frames físicos del address space de usuario de un proceso. */
void process_free_address_space(process_t *p)
{
    if (!p || !p->page_dir) return;
    vmm_free_address_space(p->page_dir);
    p->page_dir = 0;
}

/* fork(): crea un hijo con address space copiado.
 *
 * Devuelve el PID del hijo al llamador (el "padre").
 * El hijo no regresa de fork() — el kernel lo ejecuta y vuelve aquí
 * cuando el hijo llama SYS_EXIT.
 *
 * NOTA: en el modelo actual de Trinux, fork() es sincrónico:
 * el padre espera a que el hijo termine antes de continuar.
 * Para fork() asíncrono real habría que añadir wait()/waitpid()
 * completos y un scheduler que gestione múltiples page directories. */
int process_fork(void)
{
    /* ==================================================================
     * FORK DESHABILITADO (fail-safe, v0.5.3) — ver AUDITORIA.md.
     *
     * La implementación anterior era PELIGROSA: el hijo se creaba con
     * context.eip = 0; quedaba READY en la run queue y al ser elegido por
     * el scheduler, context_switch() saltaba a EIP=0 -> page fault en
     * RING 0 -> KERNEL PANIC. fork_() desde ring 3 = botón de apagado.
     *
     * Bosquejo para implementarlo bien:
     *   1. En SYS_FORK, copiar el trap frame COMPLETO del int 0x80 en
     *      curso (ds, pusha, int/err, eip/cs/eflags/useresp/ss) a la cima
     *      del kstack del hijo.
     *   2. eax=0 en la copia (retorno de fork en el hijo).
     *   3. child->context = { .eip = fork_child_entry, .esp = &copia },
     *      donde fork_child_entry (asm) hace jmp a la cola de retorno a
     *      ring 3 de syscall_stub (pop ds / popa / iret).
     *   4. Deep-copy de las page TABLES de usuario (hoy los PDs de proceso
     *      son shallow copies de las mismas tablas: padre e hijo seguirían
     *      compartiendo memoria física pese a los frames copiados).
     *   5. Cargar CR3=child_pd al entrar a ring 3 (hoy todo ring-3 corre
     *      sobre el CR3 del kernel, ver usermode_save_and_enter).
     * ================================================================== */
    kprintf("[fork] fork() no está implementado todavía (ver AUDITORIA.md)\n");
    return -1;

#if 0  /* --- implementación anterior (insegura), conservada de referencia --- */
    /* 1. Crear nuevo address space clonando el kernel identity-map */
    uint32_t child_pd = vmm_create_address_space();
    if (!child_pd) {
        kprintf("[fork] vmm_create_address_space failed\n");
        return -1;
    }

    /* 2. Copiar la región de usuario al nuevo address space */
    vmm_copy_region(child_pd, USER_COPY_START, USER_COPY_END);

    /* 3. Crear el proceso hijo */
    process_t *child = process_create("(fork-child)", NULL);
    if (!child) {
        vmm_free_address_space(child_pd);
        kprintf("[fork] process_create failed\n");
        return -1;
    }

    /* Asignar page directory propio */
    child->page_dir = child_pd;

    /* Heredar el cwd del padre */
    process_t *parent = process_get_current();
    if (parent) {
        strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);
        /* Heredar también el estado del heap (brk): vmm_copy_region() ya
         * copió físicamente el contenido de esas páginas de heap, así
         * que el hijo debe seguir viendo el mismo heap_start/heap_brk
         * que el padre tenía en el momento del fork -- si no, un SYS_BRK
         * posterior en el hijo usaría heap_start=0 (rechazado) o volvería
         * a arrancar desde 0, perdiendo la noción de cuánto heap ya
         * estaba mapeado y potencialmente re-mapeando páginas que ya
         * existen (inofensivo pero incorrecto) o dejando "agujeros". */
        child->heap_start = parent->heap_start;
        child->heap_brk   = parent->heap_brk;
    }

    int child_pid = (int)child->pid;

    /* 4. Ejecutar el hijo: cambiar al address space del hijo,
     *    ejecutarlo hasta que haga SYS_EXIT, luego volver al padre. */
    uint32_t parent_pd = vmm_get_current_dir();
    process_t *saved_current = process_get_current();

    process_set_current(child);
    vmm_switch_address_space(child_pd);

    /* El hijo corre: el scheduler lo ejecutará normalmente.
     * Como Trinux usa el flujo del kernel para la shell, el "hijo"
     * aquí es realmente el proceso recién creado que el scheduler
     * ejecutará hasta que llame process_exit(). */
    scheduler_add(child);

    /* Restaurar contexto del padre */
    process_set_current(saved_current);
    vmm_switch_address_space(parent_pd);

    return child_pid;
#endif /* 0 — implementación anterior (insegura) */
}
