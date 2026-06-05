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
extern void scheduler_set_last_user(int was_user);   /* see scheduler.c */

static void timer_callback(registers_t *regs)
{
    ticks++;
    /* Inspect the saved CS to decide whether the CPU was in ring 3 (user)
     * or ring 0 (kernel) at the moment the timer fired.  This is what
     * lets `top` show user vs system CPU time per process, htop-style. */
    int was_user = (regs && (regs->cs & 3) == 3);
    scheduler_set_last_user(was_user);
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

/* Same as sleep() but tags the current process as PROC_SLEEPING with a
 * wake-up deadline, so the scheduler can pick OTHER ready tasks while
 * we wait.  Falls back to plain sleep() if the kernel doesn't have a
 * real schedulable current task (which is true for the shell flow
 * itself, where `current` is the placeholder init kthread). */
void sleep_block(uint32_t milliseconds)
{
    extern void *process_get_current(void);
    /* We avoid pulling process.h here to keep this driver self-contained.
     * The struct layout we touch is:
     *   off 0   = pid
     *   ... etc ...
     *   off ~96 = state (4 bytes, enum)
     *   off ... = sleep_until (uint32_t)
     * Rather than fragile offsets, just call into the C side: */
    extern void scheduler_sleep_current(uint32_t until_tick);
    uint32_t target = ticks + (milliseconds * frequency) / 1000;
    scheduler_sleep_current(target);
    /* Defensive: if scheduler couldn't block (no current, ELFs running
     * in shell flow, etc.), fall through to the busy HLT loop. */
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
