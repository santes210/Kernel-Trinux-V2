/* process/process.c  -  process control blocks and bookkeeping.
 *
 * Trinux runs cooperatively in ring 0: there's no preemptive scheduling and
 * no userspace yet. But we still maintain a real process table so `ps`/`top`
 * are useful and the shell can track running commands. The boot path creates
 * the standard Unix-ish startup chain:
 *
 *   PID 1  init        (the kernel init, like /sbin/init)
 *   PID 2  kthreadd    (placeholder kernel threads container)
 *   PID 3  mysh        (the interactive shell)
 *   PID 4+ <command>   (created/destroyed by the shell per command)
 */
#include "process.h"
#include "../lib/string.h"
#include "../lib/printf.h"

static process_t  processes[MAX_PROCESSES];
static uint32_t   proc_count;
static uint32_t   next_pid = 1;
static process_t *current;

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

/* internal helper: add a process to the table without an entry point */
static process_t *spawn_kthread(const char *name, proc_state_t st)
{
    if (proc_count >= MAX_PROCESSES)
        return NULL;
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

    /* PID 1: init — the root of the process tree, like real Unix. */
    process_t *init = spawn_kthread("init", PROC_RUNNING);
    current = init;

    /* PID 2: kthreadd — represents kernel-internal work (timers, IRQs,
     * the diskfs/blockfs subsystems). It "sleeps" until something happens. */
    spawn_kthread("kthreadd", PROC_SLEEPING);

    /* PID 3: mysh — the interactive shell. Marked READY; the shell itself
     * runs cooperatively on top of init's context, but exposing it here
     * makes `ps`/`top` show the user what they expect to see. */
    spawn_kthread("mysh", PROC_READY);
}

process_t *process_create(const char *name, void (*entry)(void))
{
    if (proc_count >= MAX_PROCESSES) {
        /* Recycle the oldest zombie slot if the table is full. */
        for (uint32_t i = 0; i < proc_count; i++) {
            if (processes[i].state == PROC_ZOMBIE) {
                process_t *p = &processes[i];
                memset(p, 0, sizeof(process_t));
                p->pid = next_pid++;
                strncpy(p->name, name, PROC_NAME_MAX - 1);
                p->state = PROC_READY;
                p->entry = entry;
                strcpy(p->cwd, "/");
                return p;
            }
        }
        return NULL;
    }
    process_t *p = &processes[proc_count++];
    memset(p, 0, sizeof(process_t));
    p->pid = next_pid++;
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    p->state = PROC_READY;
    p->entry = entry;
    strcpy(p->cwd, "/");
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
    if (pid == 1)
        return -1;   /* cannot kill init */
    process_t *p = process_get(pid);
    if (!p)
        return -2;
    p->state = PROC_ZOMBIE;
    return 0;
}

uint32_t process_count(void)
{
    /* skip zombies in the visible count */
    uint32_t n = 0;
    for (uint32_t i = 0; i < proc_count; i++)
        if (processes[i].state != PROC_ZOMBIE) n++;
    return n;
}

process_t *process_at(uint32_t index)
{
    /* skip zombies so iteration over [0..count) gives only live procs */
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

