#ifndef PROCESS_SCHEDULER_H
#define PROCESS_SCHEDULER_H

#include "process.h"

void scheduler_init(void);
void scheduler_add(process_t *proc);
void scheduler_tick(void);
void schedule(void);

#endif /* PROCESS_SCHEDULER_H */
