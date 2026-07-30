#include "process.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../mm/kheap.h"
#include "../drivers/timer.h"   /* for timer_get_ticks() (start_tick) */
#include "../mm/vmm.h"         /* for vmm_create/free/switch_address_space */
#include "../auth/users.h"     /* for current_user() (root check in process_set_priority) */

static process_t  processes[MAX_PROCESSES];
static uint32_t   proc_count;
static uint32_t   next_pid = 1;
process_t *current;

/* `nice <prio> <cmd>` stashes a value here; the next process_create()
 * picks it up and resets it back to "none" (INT32_MIN). */
#define NICE_NONE  (-0x80000000)
static int        next_priority_hint = NICE_NONE;

void process_set_next_priority(int prio)
{
    if (prio < PRIO_MIN) prio = PRIO_MIN;
    if (prio > PRIO_MAX) prio = PRIO_MAX;
    next_priority_hint = prio;
}

/* The idle task — runs when literally no one else is READY.  Body is just
 * `sti; hlt` in an infinite loop: the CPU goes to sleep until the next
 * interrupt (timer tick, keyboard, serial...).  This is what actually
 * STOPS the CPU from spinning and heating up when the system is idle. */
static void idle_task_entry(void)
{
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

static const char *state_name(proc_state_t s)
{
    switch (s) {
    case PROC_RUNNING:  return "running";
    case PROC_READY:    return "ready";
    case PROC_SLEEPING: return "sleeping";
    case PROC_ZOMBIE:   return "zombie";
    default:            return "?";
    }
}

static process_t *spawn_kthread(const char *name, proc_state_t st)
{
    if (proc_count >= MAX_PROCESSES) return NULL;
    process_t *p = &processes[proc_count++];
    memset(p, 0, sizeof(process_t));   /* zeros all the new fields too */
    p->pid = next_pid++;
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    p->state = st;
    p->priority = PRIO_DEFAULT;
    p->start_tick = timer_get_ticks();
    strcpy(p->cwd, "/");
    return p;
}

/* Forward-declare the scheduler add-hook so process_init can register the
 * idle task with the run queue right after spawning it. */
extern void scheduler_add(process_t *proc);

void process_init(void)
{
    proc_count = 0;
    next_pid = 1;
    current = NULL;

    /* `init`, `kthreadd` and `mysh` are placeholder kthreads — they have
     * no real entry point and don't actually run code on a per-tick basis;
     * they exist so `ps`/`top` look familiar from the user side.  Park
     * them at PRIO_IDLE so the real idle task (which HLTs the CPU) gets
     * the CPU whenever there's nothing else to do, and so a freshly-
     * created user task at PRIO_DEFAULT instantly beats them all. */
    process_t *init = spawn_kthread("init", PROC_RUNNING);
    init->priority = PRIO_IDLE;
    current = init;

    process_t *kth = spawn_kthread("kthreadd", PROC_SLEEPING);
    kth->priority = PRIO_IDLE;

    process_t *msh = spawn_kthread("mysh", PROC_READY);
    msh->priority = PRIO_IDLE;

    /* ---- Idle "task" ----
     *
     * In a textbook OS this would be a real kthread sitting at the bottom
     * of the run queue running `hlt` forever.  But Trinux's "shell" is
     * actually the kernel's main flow (not a real schedulable task), so
     * if the scheduler ever context-switches AWAY from that flow to the
     * idle kthread, the shell never comes back.
     *
     * Pragmatic compromise: we create the idle slot as a PLACEHOLDER
     * (no kstack, never added to the run queue) and use it purely for
     * CPU-accounting purposes.  Every tick that's actually a HLT tick
     * (i.e. the shell flow is parked in keyboard_getchar() / sleep())
     * gets billed to this idle slot so that `top` shows the truth
     * "the CPU is idle 99% of the time" instead of "init is at 100%".
     *
     * Real preemption between user-space ELFs and other real schedulable
     * tasks still works; only the placeholder idle task is special. */
    process_t *idle = spawn_kthread("idle", PROC_READY);
    if (idle) idle->priority = PRIO_IDLE;
}

extern void kernel_thread_exit(void);

process_t *process_create(const char *name, void (*entry)(void))
{
    process_t* p = NULL;

    /* Always prefer to reuse a ZOMBIE slot over allocating a new one.
     * This keeps the process table compact and stops it from filling up
     * after a handful of `exec` calls (Phase A's run-of-mt test made the
     * table grow forever even though each `mt` only lived for a few ms). */
    for (uint32_t i = 0; i < proc_count; i++) {
        if (processes[i].state == PROC_ZOMBIE) {
            p = &processes[i];
            if (p->kstack) kfree(p->kstack);
            if (p->page_dir) vmm_free_address_space(p->page_dir);
            break;
        }
    }
    if (!p) {
        if (proc_count >= MAX_PROCESSES) return NULL;
        p = &processes[proc_count++];
    }

    memset(p, 0, sizeof(process_t));
    p->pid = next_pid++;
    /* Track parent PID: current process is the parent (0 for init) */
    p->parent_pid = current ? current->pid : 0;
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    p->state = PROC_READY;
    p->entry = entry;
    p->priority   = (next_priority_hint != NICE_NONE) ? next_priority_hint
                                                      : PRIO_DEFAULT;
    next_priority_hint = NICE_NONE;
    p->dyn_boost      = 0;
    p->cpu_ticks      = 0;
    p->cpu_ticks_user = 0;
    p->cpu_ticks_sys  = 0;
    p->ticks_window   = 0;
    p->quantum_used   = 0;
    p->consec_full    = 0;
    p->sleep_until    = 0;
    p->start_tick     = timer_get_ticks();
    strcpy(p->cwd, "/");

    /* Create a new address space for this process */
    p->page_dir = vmm_create_address_space();
    if (!p->page_dir) {
        kprintf("[process] WARNING: failed to create address space for PID %u\n", p->pid);
        /* Continue without dedicated address space (will use kernel's) */
    }

    p->kstack = kmalloc_aligned(8192);
    uint32_t* stack = (uint32_t*)((uint32_t)p->kstack + 8192);
    
    /* Simulate a call frame for context_switch to ret into */
    *(--stack) = (uint32_t)kernel_thread_exit; /* where to return if entry() returns */
    *(--stack) = (uint32_t)entry;              /* eip */
    
    p->context.eip = (uint32_t)entry;
    p->context.esp = (uint32_t)stack;
    p->context.ebp = (uint32_t)stack;
    p->context.eflags = 0x202; /* IF enabled */

    return p;
}

void process_exit(int code)
{
    if (current) {
        current->exit_code = code;
        current->state = PROC_ZOMBIE;

        /* CRITICAL: Close all file descriptors owned by this process
         * to prevent FD leaks that would exhaust the global fd table. */
        extern void fd_cleanup_process(uint32_t pid);
        fd_cleanup_process(current->pid);

        /* CRITICAL: Reparent orphan children to init (PID 1).
         * When a parent dies before its children, the children become
         * orphans. In POSIX, init adopts them and reaps them when they die. */
        for (uint32_t i = 0; i < proc_count; i++) {
            if (processes[i].state != PROC_ZOMBIE &&
                processes[i].parent_pid == current->pid &&
                processes[i].pid != 1) {
                processes[i].parent_pid = 1;  /* Adopt by init */
            }
        }

        /* Enviar SIGCHLD al padre (si existe y no es init) */
        if (current->parent_pid > 0) {
            process_signal(current->parent_pid, SIGCHLD);
        }

        /* CRITICAL: Auto-reap zombies if table is getting full.
         * If >75% of process slots are zombies, reap orphaned zombies
         * (those whose parent is also dead) to prevent table exhaustion. */
        int zombie_count = 0;
        for (uint32_t i = 0; i < proc_count; i++) {
            if (processes[i].state == PROC_ZOMBIE) zombie_count++;
        }
        if (zombie_count > (int)(proc_count * 3 / 4) && proc_count > 8) {
            for (uint32_t i = 0; i < proc_count; i++) {
                if (processes[i].state == PROC_ZOMBIE && processes[i].pid > 3) {
                    process_t *parent = process_get(processes[i].parent_pid);
                    if (!parent || parent->state == PROC_ZOMBIE) {
                        /* Parent is dead or doesn't exist, safe to reap.
                         * We mark the name as empty so process_create can
                         * reuse this slot (memset in process_create clears it). */
                        processes[i].name[0] = '\0';
                    }
                }
            }
        }

        /* Free the process's address space (will be fully freed when slot is reused) */
        if (current->page_dir) {
            vmm_free_address_space(current->page_dir);
            current->page_dir = 0;
        }
    }
}

process_t *process_get_current(void) { return current; }

process_t *process_get(uint32_t pid)
{
    for (uint32_t i = 0; i < proc_count; i++)
        if (processes[i].pid == pid && processes[i].state != PROC_ZOMBIE)
            return &processes[i];
    return NULL;
}

int process_kill(uint32_t pid)
{
    if (pid == 1) return -1;
    process_t *p = process_get(pid);
    if (!p) return -2;
    p->state = PROC_ZOMBIE;
    return 0;
}

uint32_t process_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < proc_count; i++)
        if (processes[i].state != PROC_ZOMBIE) n++;
    return n;
}

process_t *process_at(uint32_t index)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; i < proc_count; i++) {
        if (processes[i].state == PROC_ZOMBIE) continue;
        if (seen == index) return &processes[i];
        seen++;
    }
    return NULL;
}

void process_list(void)
{
    kprintf("  PID  PRI  STATE     TIME+      NAME\n");
    kprintf("  ---  ---  --------  ---------  ----------------\n");
    uint32_t hz = timer_get_freq();
    if (hz == 0) hz = 100;
    for (uint32_t i = 0; i < proc_count; i++) {
        process_t *p = &processes[i];
        if (p->state == PROC_ZOMBIE) continue;
        uint32_t total_sec = p->cpu_ticks / hz;
        uint32_t total_cs  = (p->cpu_ticks * 100 / hz) % 100;
        if (p->priority >= PRIO_IDLE)
            kprintf("  %-3u  idl  %-8s  %u:%02u.%02u    %s\n",
                    p->pid, state_name(p->state),
                    total_sec / 60, total_sec % 60, total_cs, p->name);
        else
            kprintf("  %-3u  %3d  %-8s  %u:%02u.%02u    %s\n",
                    p->pid, p->priority, state_name(p->state),
                    total_sec / 60, total_sec % 60, total_cs, p->name);
    }
}

void process_set_current(process_t* p) {
    extern process_t *current;
    current = p;
}

/* Cosmetic placeholder, doesn't disturb the pending `nice` hint.  See the
 * comment in process.h for why this matters. */
process_t *process_create_tracking(const char *name)
{
    int saved_hint = next_priority_hint;
    next_priority_hint = NICE_NONE;
    process_t *p = process_create(name, NULL);
    next_priority_hint = saved_hint;
    return p;
}

int process_set_priority(uint32_t pid, int prio)
{
    if (prio < PRIO_MIN || prio > PRIO_MAX) return -2;
    process_t *p = process_get(pid);
    if (!p) return -1;
    /* SECURITY: boosting priority (any value below the default, i.e. more
     * negative = more CPU share) is root-only, exactly like Unix `nice`.
     * Before this check, ANY unprivileged process could call renice_(pid,
     * -20) via SYS_RENICE on itself (or on any other pid, including PID 1)
     * and starve the whole system -- a trivial denial-of-service. Raising
     * your own niceness (prio >= PRIO_DEFAULT, i.e. being *more* polite)
     * remains unrestricted, same as real Unix. */
    if (prio < PRIO_DEFAULT) {
        user_t *u = current_user();
        if (!u || u->uid != 0)
            return -3;
    }
    p->priority = prio;
    return 0;
}

/* ---- Signals (v0.3) ---- */

int process_signal(uint32_t pid, int sig)
{
    process_t *p = process_get(pid);
    if (!p) return -1;
    if (p->state == PROC_ZOMBIE) return -1;
    p->signal_pending = sig;
    return 0;
}

bool process_check_signal(void)
{
    process_t *p = process_get_current();
    if (!p) return false;
    if (!p->signal_pending) return false;

    int sig = p->signal_pending;
    p->signal_pending = 0;

    /* Usar el sistema de entrega de señales que soporta handlers */
    return process_deliver_signal(sig);
}

/* ---- fork/waitpid (v0.3.1) ---- */

uint32_t process_get_ppid(uint32_t pid)
{
    process_t *p = process_get(pid);
    if (!p) return 0;
    return p->parent_pid;
}

/* waitpid: bloquea al proceso actual hasta que un hijo termine.
 *
 * pid > 0:  espera a ese hijo específico
 * pid == -1: espera a cualquier hijo
 * options & WNOHANG: no bloquea si no hay hijos terminados
 *
 * Devuelve: PID del hijo terminado, 0 si WNOHANG y no hay, -1 error.
 * *status recibe el exit_code del hijo.
 */
int process_waitpid(int pid, int *status, int options)
{
    process_t *me = process_get_current();
    if (!me) return -1;

    for (;;) {
        /* Buscar un hijo zombie que matchee el criterio */
        for (uint32_t i = 0; i < proc_count; i++) {
            process_t *child = &processes[i];
            if (child->parent_pid != me->pid) continue;
            if (pid > 0 && child->pid != (uint32_t)pid) continue;
            if (child->state == PROC_ZOMBIE) {
                int child_pid = (int)child->pid;
                if (status) *status = child->exit_code;
                /* El slot será reciclado en el próximo process_create */
                return child_pid;
            }
        }

        /* Si WNOHANG y no encontramos zombie, retornar 0 */
        if (options & 1) return 0;  /* WNOHANG */

        /* Verificar si tenemos algún hijo vivo */
        bool has_children = false;
        for (uint32_t i = 0; i < proc_count; i++) {
            if (processes[i].parent_pid == me->pid &&
                processes[i].state != PROC_ZOMBIE) {
                has_children = true;
                break;
            }
        }
        if (!has_children) return -1;  /* ECHILD */

        /* Bloquear: yield y reintentar */
        schedule();
    }
}

/* ---- Signal handlers (v0.3.2) ---- */

void (*process_sigaction(int sig, void (*handler)(int)))(int)
{
    /* SIGKILL no puede ser capturado */
    if (sig == SIGKILL || sig <= 0 || sig >= _NSIG)
        return SIG_DFL;

    process_t *p = process_get_current();
    if (!p) return SIG_DFL;

    void (*old)(int) = p->sig_handlers[sig];
    p->sig_handlers[sig] = handler;
    return old ? old : SIG_DFL;
}

bool process_deliver_signal(int sig)
{
    process_t *p = process_get_current();
    if (!p) return true;  /* no process = terminate */

    if (sig <= 0 || sig >= _NSIG) return true;

    void (*handler)(int) = p->sig_handlers[sig];

    /* SIG_IGN: ignorar la señal */
    if (handler == SIG_IGN) return false;

    /* Handler personalizado: llamarlo (será ejecutado en ring 3) */
    if (handler && handler != SIG_DFL) {
        /* En un SO completo, aquí haríamos un trampoline que:
         * 1. Guarda el contexto actual del proceso
         * 2. Ejecuta el handler en ring 3
         * 3. Restaura el contexto y vuelve a donde estaba
         *
         * Por ahora, registramos que hay un handler pendiente
         * y el syscall handler lo llamará antes de volver a ring 3.
         * Esto es simplificado pero funcional. */
        p->signal_pending = sig;
        return false;  /* no terminar, hay handler */
    }

    /* SIG_DFL o NULL: acción por defecto (terminar) */
    /* SIGCHLD por defecto se ignora */
    if (sig == SIGCHLD) return false;

    kprintf("\n[%s] terminado por señal %d\n", p->name, sig);
    p->state = PROC_ZOMBIE;
    p->exit_code = 128 + sig;
    p->signaled = true;
    return true;  /* proceso terminado */
}
