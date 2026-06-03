/* drivers/timer.c  -  Programmable Interval Timer (IRQ0). */
#include "timer.h"
#include "../cpu/irq.h"
#include "../cpu/ports.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQ     1193182

static volatile uint32_t ticks;
static volatile uint32_t idle_ticks;   /* incremented when CPU was in HLT */
static uint32_t frequency = 100;

/* snapshot for CPU usage calculation */
static volatile uint32_t last_ticks;
static volatile uint32_t last_idle;

/* scheduler hook (weak-ish: defined in scheduler.c) */
extern void scheduler_tick(void);

static void timer_callback(registers_t *regs)
{
    (void)regs;
    ticks++;
    scheduler_tick();
}

void timer_init(uint32_t freq)
{
    if (freq == 0)
        freq = 100;
    frequency = freq;

    uint32_t divisor = PIT_FREQ / freq;

    outb(PIT_COMMAND, 0x36);                      /* channel 0, lobyte/hibyte, mode 3 */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_register_handler(0, timer_callback);
}

uint32_t timer_get_ticks(void) { return ticks; }
uint32_t timer_get_freq(void)  { return frequency; }

void sleep(uint32_t milliseconds)
{
    uint32_t target = ticks + (milliseconds * frequency) / 1000;
    /* ensure interrupts are on so ticks advance */
    __asm__ volatile("sti");
    while (ticks < target) {
        idle_ticks++;
        __asm__ volatile("hlt");
    }
}

/* Return approximate CPU usage 0-100 since last call. */
uint32_t timer_cpu_usage(void)
{
    uint32_t dt = ticks - last_ticks;
    uint32_t di = idle_ticks - last_idle;
    last_ticks = ticks;
    last_idle  = idle_ticks;
    if (dt == 0) return 0;
    uint32_t idle_pct = (di * 100) / dt;
    return (idle_pct > 100) ? 0 : (100 - idle_pct);
}

uint32_t uptime(void)
{
    return ticks / frequency;
}
