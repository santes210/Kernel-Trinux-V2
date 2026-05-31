/* process/scheduler.c  -  round-robin scheduler skeleton.
 *
 * The context-switch primitive (context_switch in switch.asm) is implemented
 * and available. By design (boot stability) the scheduler runs cooperatively:
 * scheduler_tick() simply accounts time and the system runs the interactive
 * shell as PID 1. schedule() picks the next READY process round-robin when
 * called explicitly. This keeps the kernel stable while exposing the full
 * scheduling API (init/add/tick/schedule + ASM context switch).
 */
#include "scheduler.h"
#include "../lib/types.h"

extern void context_switch(context_t *old, context_t *new);

static process_t *run_queue[MAX_PROCESSES];
static uint32_t   queue_len;
static uint32_t   current_idx;
static volatile uint32_t tick_count;

void scheduler_init(void)
{
    queue_len = 0;
    current_idx = 0;
    tick_count = 0;
}

void scheduler_add(process_t *proc)
{
    if (queue_len < MAX_PROCESSES)
        run_queue[queue_len++] = proc;
}

/* Called from the timer IRQ at 100 Hz. Cooperative: just count ticks. */
void scheduler_tick(void)
{
    tick_count++;
}

/* Round-robin selection (cooperative). */
void schedule(void)
{
    if (queue_len == 0)
        return;

    uint32_t start = current_idx;
    do {
        current_idx = (current_idx + 1) % queue_len;
        process_t *p = run_queue[current_idx];
        if (p && p->state == PROC_READY) {
            process_t *prev = run_queue[start];
            p->state = PROC_RUNNING;
            if (prev && prev != p && prev->state == PROC_RUNNING)
                prev->state = PROC_READY;
            context_switch(prev ? &prev->context : NULL, &p->context);
            return;
        }
    } while (current_idx != start);
}
