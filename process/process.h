#ifndef PROCESS_PROCESS_H
#define PROCESS_PROCESS_H

#include "../lib/types.h"

typedef enum {
    PROC_RUNNING,
    PROC_READY,
    PROC_SLEEPING,
    PROC_ZOMBIE
} proc_state_t;

#define PROC_NAME_MAX 32
#define MAX_PROCESSES 128

/* Priority range — same convention as Unix `nice`:
 *     -20 = highest priority   (gets CPU first, can starve others)
 *       0 = default
 *     +19 = lowest  priority   (only runs when nobody else wants to)
 *
 * The idle task lives one step below the userland minimum at PRIO_IDLE and
 * is never selected unless every other ready task is below it. */
#define PRIO_MIN     (-20)
#define PRIO_MAX     ( 19)
#define PRIO_DEFAULT (  0)
#define PRIO_IDLE    (100)        /* internal — not reachable via `nice` */

/* Saved CPU context for cooperative/round-robin switching. */
typedef struct {
    uint32_t esp, ebp, ebx, esi, edi, eflags, eip;
} context_t;

typedef struct process {
    uint32_t      pid;
    char          name[PROC_NAME_MAX];
    proc_state_t  state;
    context_t     context;
    uint32_t      page_dir;       /* physical address of page directory */
    char          cwd[256];
    int           exit_code;
    void        (*entry)(void);
    void*         kstack;         /* kernel stack for multitasking */

    /* ---- scheduling accounting ---- */
    int           priority;       /* base prio (-20..+19), set by nice()    */
    int           dyn_boost;      /* MLFQ adjustment, set by scheduler      */
    uint32_t      cpu_ticks;      /* total ticks ever spent in RUNNING      */
    uint32_t      cpu_ticks_user; /* of cpu_ticks, those spent in ring 3    */
    uint32_t      cpu_ticks_sys;  /* of cpu_ticks, those spent in ring 0    */
    uint32_t      ticks_window;   /* ticks spent in this %CPU sample window */
    uint32_t      start_tick;     /* timer_get_ticks() at creation time     */
    uint32_t      quantum_used;   /* ticks consumed in the CURRENT quantum  */
    uint32_t      consec_full;    /* consecutive times quantum hit the cap  */
    uint32_t      sleep_until;    /* tick at which a sleeper should wake    */
} process_t;

void        process_init(void);
process_t  *process_create(const char *name, void (*entry)(void));
void        process_exit(int code);
process_t  *process_get_current(void);
void        process_list(void);        /* prints active processes */
process_t  *process_get(uint32_t pid);
int         process_kill(uint32_t pid);
uint32_t    process_count(void);
process_t  *process_at(uint32_t index);

/* Adjust a process' priority.  Returns 0 on success, -1 if PID invalid,
 * -2 if `prio` is out of range, -3 if the caller is not root (root has
 * uid 0 and is the only one allowed to LOWER priority numbers, i.e.
 * boost a task). */
int         process_set_priority(uint32_t pid, int prio);

/* `nice <prio> <cmd>` plumbing: the next process_create() call (and only
 * that one) will start at this priority instead of PRIO_DEFAULT. */
void        process_set_next_priority(int prio);

/* Like process_create() but explicitly tagged as a cosmetic tracking slot
 * that should NOT consume the pending `nice` hint.  commands_dispatch()
 * uses this for the placeholder process_t it creates for every built-in
 * command, so that `nice -5 exec /bin/foo` reaches the real ELF instead
 * of being absorbed by the throwaway `exec` slot a few lines earlier. */
process_t  *process_create_tracking(const char *name);

/* Re-point the kernel-wide "current task" to `p`.  Used by elf_exec()
 * to make a freshly-spawned ELF the active task while it runs, then
 * restore the shell when it returns or longjmps out. */
void        process_set_current(process_t *p);

#endif /* PROCESS_PROCESS_H */
