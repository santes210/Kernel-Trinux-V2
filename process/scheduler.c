/* process/scheduler.c  -  Priority-based preemptive scheduler with MLFQ.
 *
 * Phase A (priority + idle task) is unchanged: every process has a base
 * priority in the Unix `nice` style and the idle task lives at PRIO_IDLE
 * so it only runs when nobody else wants the CPU.
 *
 * Phase B adds three things on top:
 *
 *   1. **MLFQ feedback** (`dyn_boost`).  An integer adjustment applied
 *      on top of `priority` for the next scheduling decision.  Rules:
 *        - When a task voluntarily releases the CPU early (sleep, block
 *          on I/O, etc.) we credit it +1 (= lower effective priority
 *          number = boosted) up to MLFQ_BOOST_MAX.  This is what makes
 *          interactive tasks like the editor "feel snappy".
 *        - When a task burns through its entire quantum N times in a row
 *          we charge -1 (= higher effective priority number = demoted),
 *          down to MLFQ_BOOST_MIN.  CPU-bound batch work naturally sinks
 *          to the bottom and stops starving everything else.
 *        - Every MLFQ_DECAY_TICKS we halve all non-zero `dyn_boost`
 *          values, so a process that *used* to be interactive doesn't
 *          keep its priority forever once its workload changes.
 *
 *   2. **Sleeping & wakeups** (`sleep_until`, `scheduler_wakeup_check`).
 *      The PIT IRQ runs every tick and re-marks any PROC_SLEEPING task
 *      whose `sleep_until` has expired as PROC_READY.  Combined with
 *      `scheduler_kick()`, an IRQ that wakes a high-priority task
 *      preempts the current task on the very next tick.
 *
 *   3. **Honest CPU accounting** (`looks_like_idle_period`).  The shell
 *      lives in init's flow at PRIO_IDLE, so ticks that happen while
 *      "init is RUNNING" are really HLT ticks.  Bill them to the real
 *      `idle` task so `top` shows the truth: the CPU is asleep.
 *
 * The scheduler API itself (scheduler_init / scheduler_add /
 * scheduler_tick / schedule / scheduler_kick) is unchanged so the rest
 * of the kernel doesn't need to know.
 */
#include "scheduler.h"
#include "../lib/types.h"
#include "../drivers/timer.h"

extern void context_switch(context_t *old, context_t *new);
extern void process_set_current(process_t* p);
extern process_t* process_get_current(void);

static process_t *run_queue[MAX_PROCESSES];
static uint32_t   queue_len;
static uint32_t   last_picked_idx;          /* round-robin tiebreaker */
static volatile uint32_t tick_count;
static volatile uint32_t kick_flag;          /* set by scheduler_kick() */
static volatile int      last_was_user;     /* set by timer_callback */

/* Called by the timer IRQ right BEFORE scheduler_tick(), to record
 * whether the interrupted code was running in ring 3.  We use this to
 * split cpu_ticks into user vs system time. */
void scheduler_set_last_user(int was_user)
{
    last_was_user = was_user;
}

/* ---- MLFQ tuning knobs ---- */
#define MLFQ_BOOST_MAX        4    /* max upward adjustment            */
#define MLFQ_BOOST_MIN      (-4)   /* max downward adjustment          */
#define MLFQ_DEMOTE_AFTER     2    /* full-quantum streaks before demote */
#define MLFQ_DECAY_TICKS    500    /* every 5 s @100 Hz: halve all boosts */

void scheduler_init(void)
{
    queue_len = 0;
    last_picked_idx = 0;
    tick_count = 0;
    kick_flag = 0;
}

void scheduler_add(process_t *proc)
{
    if (queue_len < MAX_PROCESSES)
        run_queue[queue_len++] = proc;
}

/* Effective priority = base + dyn_boost.  Lower number = more important. */
static int eff_prio(const process_t *p)
{
    int e = p->priority + p->dyn_boost;
    if (p->priority >= PRIO_IDLE) return p->priority;  /* idle stays idle */
    if (e < PRIO_MIN) e = PRIO_MIN;
    if (e > PRIO_MAX) e = PRIO_MAX;
    return e;
}

/* Convert an EFFECTIVE priority into the number of timer ticks the task
 * is allowed to keep the CPU before we even consider preempting it.  At
 * 100 Hz:
 *     PRIO_MIN  (-20) -> 2 ticks  = 20 ms   (super responsive)
 *     PRIO_DEFAULT(0) -> 5 ticks  = 50 ms   (= old behaviour)
 *     PRIO_MAX  (+19) -> 12 ticks = 120 ms  (less switch overhead)
 *     PRIO_IDLE       -> 1 tick             (yield ASAP when work arrives) */
static uint32_t quantum_for(int prio)
{
    if (prio >= PRIO_IDLE) return 1;
    int q = 5 + (prio / 4);
    if (q < 2)  q = 2;
    if (q > 12) q = 12;
    return (uint32_t)q;
}

/* Find the dedicated `idle` task by name in the global process table.
 * Cached after first lookup.  This deliberately doesn't look at the
 * scheduler's run queue: the idle task is a placeholder for CPU
 * accounting and is NEVER context-switched to (see the long comment in
 * process_init()). */
extern process_t *process_get(uint32_t pid);
extern uint32_t   process_count(void);
extern process_t *process_at(uint32_t index);

static process_t *cached_idle = NULL;
static process_t *find_idle_task(void)
{
    if (cached_idle) return cached_idle;
    uint32_t n = process_count();
    for (uint32_t i = 0; i < n; i++) {
        process_t *p = process_at(i);
        if (!p) continue;
        /* Match by name to avoid grabbing the wrong placeholder. */
        if (p->name[0] == 'i' && p->name[1] == 'd' &&
            p->name[2] == 'l' && p->name[3] == 'e' && p->name[4] == 0) {
            cached_idle = p;
            return p;
        }
    }
    return NULL;
}

/* Heuristic: are we currently in a "kernel is just waiting for input" tick?
 * The shell + early kthreads all sit at PRIO_IDLE because they're not real
 * schedulable tasks; ticks attributed to them when no real work is queued
 * really represent CPU idle time. */
static int looks_like_idle_period(process_t *cur)
{
    if (!cur) return 1;
    if (cur->priority >= PRIO_IDLE) return 1;
    return 0;
}

/* MLFQ: a task that voluntarily yielded EARLY (used less than its full
 * quantum and ended in SLEEPING/READY rather than RUNNING) gets a +1
 * boost.  Called by the scheduler when it actually replaces `prev`. */
static void mlfq_credit_release(process_t *p, int used, int cap)
{
    if (!p) return;
    if (p->priority >= PRIO_IDLE) return;
    if (used < cap) {
        if (p->dyn_boost > MLFQ_BOOST_MIN)
            p->dyn_boost--;          /* -1 in dyn_boost = lower number = boost */
        p->consec_full = 0;
    }
}

/* MLFQ: a task that exhausted its quantum N consecutive times gets a -1
 * boost (demoted).  Called when scheduler_tick sees `quantum_used >= cap`. */
static void mlfq_charge_quantum(process_t *p, int cap)
{
    if (!p) return;
    if (p->priority >= PRIO_IDLE) return;
    if ((int)p->quantum_used >= cap) {
        p->consec_full++;
        if (p->consec_full >= MLFQ_DEMOTE_AFTER) {
            if (p->dyn_boost < MLFQ_BOOST_MAX)
                p->dyn_boost++;      /* +1 = higher number = demoted */
            p->consec_full = 0;
        }
    }
}

/* Periodic decay so a once-interactive task doesn't keep its boost
 * forever after switching to CPU-bound work.  Halves every dyn_boost. */
static void mlfq_decay_all(void)
{
    for (uint32_t i = 0; i < queue_len; i++) {
        process_t *p = run_queue[i];
        if (!p) continue;
        if (p->dyn_boost > 0) p->dyn_boost /= 2;
        else if (p->dyn_boost < 0) p->dyn_boost = -((-p->dyn_boost) / 2);
    }
}

/* Wake any sleeper whose deadline has passed.  Called every tick. */
static void scheduler_wakeup_check(void)
{
    uint32_t now = timer_get_ticks();
    for (uint32_t i = 0; i < queue_len; i++) {
        process_t *p = run_queue[i];
        if (!p) continue;
        if (p->state == PROC_SLEEPING && p->sleep_until &&
            now >= p->sleep_until) {
            p->state = PROC_READY;
            p->sleep_until = 0;
            kick_flag = 1;          /* force reschedule next tick */
        }
    }
}

/* Called from the timer IRQ at 100 Hz.  Bills CPU time, decays MLFQ
 * adjustments, checks for sleepers to wake, and maybe preempts. */
void scheduler_tick(void)
{
    tick_count++;

    /* MLFQ decay pass — periodic, cheap. */
    if ((tick_count % MLFQ_DECAY_TICKS) == 0)
        mlfq_decay_all();

    /* Move any sleeper whose deadline elapsed back to READY. */
    scheduler_wakeup_check();

    process_t *cur = process_get_current();
    process_t *bill_to;

    if (looks_like_idle_period(cur)) {
        bill_to = find_idle_task();
        if (!bill_to) bill_to = cur;
    } else {
        bill_to = cur;
    }

    if (bill_to && bill_to->state != PROC_ZOMBIE) {
        bill_to->cpu_ticks++;
        bill_to->ticks_window++;
        if (last_was_user) bill_to->cpu_ticks_user++;
        else               bill_to->cpu_ticks_sys++;
    }
    if (cur && cur->state != PROC_ZOMBIE)
        cur->quantum_used++;

    /* Compute the cap for the current task's effective priority. */
    int prio = cur ? eff_prio(cur) : PRIO_DEFAULT;
    uint32_t cap = quantum_for(prio);

    /* Demote the task if it just hit the cap; the demotion takes effect
     * on the NEXT scheduling decision (we don't recurse). */
    if (cur && cur->state != PROC_ZOMBIE && cur->quantum_used >= cap)
        mlfq_charge_quantum(cur, (int)cap);

    if (kick_flag || (cur && cur->quantum_used >= cap)) {
        kick_flag = 0;
        schedule();
    }
}

/* Pick the READY/RUNNING task with the lowest EFFECTIVE priority number.
 * Round-robin among ties via last_picked_idx so equal-priority tasks
 * share the CPU evenly. */
static int pick_next(void)
{
    if (queue_len == 0) return -1;

    int best = -1;
    int best_prio = 0x7FFFFFFF;

    for (uint32_t i = 0; i < queue_len; i++) {
        uint32_t idx = (last_picked_idx + 1 + i) % queue_len;
        process_t *p = run_queue[idx];
        if (!p) continue;
        if (p->state != PROC_READY && p->state != PROC_RUNNING) continue;
        int ep = eff_prio(p);
        if (ep < best_prio) {
            best_prio = ep;
            best = (int)idx;
        }
    }
    return best;
}

void schedule(void)
{
    int idx = pick_next();
    if (idx < 0) return;

    process_t *p    = run_queue[idx];
    process_t *prev = process_get_current();

    /* No-op if we picked the same task that was already running. */
    if (prev == p) {
        p->quantum_used = 0;
        return;
    }

    if (prev) {
        /* If prev released the CPU voluntarily (i.e. didn't exhaust its
         * quantum) reward it with a boost. */
        int prev_cap = (int)quantum_for(eff_prio(prev));
        mlfq_credit_release(prev, (int)prev->quantum_used, prev_cap);
        if (prev->state == PROC_RUNNING)
            prev->state = PROC_READY;
    }

    p->state = PROC_RUNNING;
    p->quantum_used = 0;

    process_set_current(p);
    last_picked_idx = (uint32_t)idx;

    /* If ring 3 is running, update the TSS stack pointer so the next
     * interrupt uses this task's kernel stack. */
    extern void tss_set_kernel_stack(uint32_t esp0);
    if (p->kstack)
        tss_set_kernel_stack((uint32_t)p->kstack + 8192);

    context_switch(prev ? &prev->context : NULL, &p->context);
}

/* Force a reschedule at the next IRQ.  Called by wakeup paths (keyboard,
 * serial, timer-based sleeper expiry) so a freshly-READY high-priority
 * task starts running immediately instead of waiting for the current
 * task to use up its quantum. */
void scheduler_kick(void)
{
    kick_flag = 1;
}

/* Mark the current task as PROC_SLEEPING with the given wake-up tick
 * and force an immediate reschedule.  Only works for tasks with a real
 * kernel stack; for placeholder kthreads this is a no-op (the timer
 * driver falls back to busy HLT in that case). */
void scheduler_sleep_current(uint32_t until_tick)
{
    process_t *cur = process_get_current();
    if (!cur || !cur->kstack) return;
    cur->sleep_until = until_tick;
    cur->state       = PROC_SLEEPING;
    /* Reset MLFQ accounting for the sleeper — it released the CPU
     * voluntarily, so it deserves the interactive boost. */
    int cap = (int)quantum_for(eff_prio(cur));
    mlfq_credit_release(cur, (int)cur->quantum_used, cap);
    cur->quantum_used = 0;
    /* Yield. */
    schedule();
}
