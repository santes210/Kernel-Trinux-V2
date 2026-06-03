#ifndef DRIVERS_TIMER_H
#define DRIVERS_TIMER_H

#include "../lib/types.h"

void     timer_init(uint32_t freq);
uint32_t timer_get_ticks(void);
uint32_t timer_get_freq(void);
void     sleep(uint32_t milliseconds);
uint32_t uptime(void);          /* seconds since boot */
uint32_t timer_cpu_usage(void); /* approx CPU usage % since last call */

#endif /* DRIVERS_TIMER_H */
