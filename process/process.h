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

    /* ---- signals (v0.3) ---- */
    int           signal_pending; /* non-zero: signal number to deliver      */
    bool          signaled;       /* true if killed by signal (exit=128+sig) */

    /* ---- fork/waitpid (v0.3.1) ---- */
    uint32_t      parent_pid;     /* PID del proceso padre (0 si init)       */
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

/* ---- Signals (v0.3) ---- */
#define SIGHUP    1
#define SIGINT    2    /* Ctrl-C */
#define SIGQUIT   3
#define SIGKILL   9    /* cannot be caught/ignored */
#define SIGPIPE  13    /* write to broken pipe */
#define SIGTERM  15

/* Deliver a signal to a process. Currently the only action is to set
 * signal_pending; the process will be terminated on the next syscall
 * return or scheduler tick. Returns 0 on success, -1 if pid invalid. */
int         process_signal(uint32_t pid, int sig);

/* Check if the current process has a pending signal. If so, terminate it
 * with exit code 128+signum. Called from syscall return path. Returns
 * true if a signal was delivered (caller should not return to usermode). */
bool        process_check_signal(void);

/* Get the parent PID of a process. Returns 0 if no parent (init). */
uint32_t    process_get_ppid(uint32_t pid);

/* Wait for a child process to exit. If pid > 0, wait for that specific
 * child. If pid == -1, wait for any child. Returns the child PID and
 * sets *status to the exit code. If WNOHANG is set and no child has
 * exited, returns 0 immediately. Returns -1 if no children or error. */
int         process_waitpid(int pid, int *status, int options);

#endif /* PROCESS_PROCESS_H */
