#include "process.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../mm/kheap.h"

static process_t  processes[MAX_PROCESSES];
static uint32_t   proc_count;
static uint32_t   next_pid = 1;
process_t *current;

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
    memset(p, 0, sizeof(process_t));
    p->pid = next_pid++;
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    p->state = st;
    strcpy(p->cwd, "/");
    return p;
}

void process_init(void)
{
    proc_count = 0;
    next_pid = 1;
    current = NULL;

    process_t *init = spawn_kthread("init", PROC_RUNNING);
    current = init;

    spawn_kthread("kthreadd", PROC_SLEEPING);
    spawn_kthread("mysh", PROC_READY);
}

extern void kernel_thread_exit(void);

process_t *process_create(const char *name, void (*entry)(void))
{
    process_t* p = NULL;
    if (proc_count >= MAX_PROCESSES) {
        for (uint32_t i = 0; i < proc_count; i++) {
            if (processes[i].state == PROC_ZOMBIE) {
                p = &processes[i];
                if (p->kstack) kfree(p->kstack);
                break;
            }
        }
        if (!p) return NULL;
    } else {
        p = &processes[proc_count++];
    }

    memset(p, 0, sizeof(process_t));
    p->pid = next_pid++;
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    p->state = PROC_READY;
    p->entry = entry;
    strcpy(p->cwd, "/");

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
    kprintf("  PID  STATE     NAME\n");
    kprintf("  ---  --------  ----------------\n");
    for (uint32_t i = 0; i < proc_count; i++) {
        process_t *p = &processes[i];
        if (p->state == PROC_ZOMBIE) continue;
        kprintf("  %-3u  %-8s  %s\n", p->pid, state_name(p->state), p->name);
    }
}

void process_set_current(process_t* p) {
    extern process_t *current;
    current = p;
}
