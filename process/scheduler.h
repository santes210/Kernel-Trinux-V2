#ifndef PROCESS_SCHEDULER_H
#define PROCESS_SCHEDULER_H

#include "process.h"

void scheduler_init(void);
void scheduler_add(process_t *proc);
void scheduler_tick(void);
void schedule(void);
void scheduler_kick(void);   /* force a reschedule at the next IRQ */

/* Mark `current` as PROC_SLEEPING with deadline `until_tick` and yield
 * to the scheduler.  No-op if `current` doesn't have a real kstack
 * (placeholder kthreads can't really block). */
void scheduler_sleep_current(uint32_t until_tick);

#endif /* PROCESS_SCHEDULER_H */
