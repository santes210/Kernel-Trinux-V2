#include "scheduler.h"
#include "../lib/types.h"

extern void context_switch(context_t *old, context_t *new);
extern void process_set_current(process_t* p);
extern process_t* process_get_current(void);

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

/* Called from the timer IRQ at 100 Hz. Preemptive scheduler! */
void scheduler_tick(void)
{
    tick_count++;
    if (tick_count % 5 == 0) { // every 50ms (assuming 100Hz)
        schedule();
    }
}

/* Round-robin selection. */
void schedule(void)
{
    if (queue_len == 0)
        return;

    uint32_t start = current_idx;
    do {
        current_idx = (current_idx + 1) % queue_len;
        process_t *p = run_queue[current_idx];
        if (p && p->state == PROC_READY) {
            process_t *prev = process_get_current();
            p->state = PROC_RUNNING;
            if (prev && prev != p && prev->state == PROC_RUNNING)
                prev->state = PROC_READY;
            
            process_set_current(p);
            
            if (prev == p) return;
            
            /* If ring 3 is running, update the TSS stack pointer so 
             * the next interrupt uses this task's kernel stack! */
            extern void tss_set_kernel_stack(uint32_t esp0);
            if (p->kstack) {
                tss_set_kernel_stack((uint32_t)p->kstack + 8192);
            }
            
            context_switch(prev ? &prev->context : NULL, &p->context);
            return;
        }
    } while (current_idx != start);
}
