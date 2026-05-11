#include "task.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "paging.h"
#include "pit.h"
#include "string.h"
#include "tss.h"
#include "isr.h"
#include "elf.h"
#include "fs.h"
#include "sock.h"
#include "pipe.h"
#include "tmpfs.h"
#include "signal.h"
#include "smp.h"
#include "spinlock.h"
#include "../include/io.h"

/* fs.h has FS_DIR_ROOT but `task.h` can't include `fs.h` cleanly
 * (would be circular). The literal 0xFF is the same constant. */
#define TASK_CWD_ROOT  0xFFu

extern void task_switch(uint32_t *old_esp, uint32_t new_esp);
extern void fork_child_return(void);     /* asm trampoline in task_switch.S */
extern void task_entry_trampoline(void); /* asm trampoline in task_switch.S */

/* Called from the trampolines above (task_entry_trampoline and
 * fork_child_return) — releases the scheduler lock the dispatching
 * schedule() acquired. Without this, a brand-new task would never
 * release the lock and the system would freeze on the next schedule. */
void post_switch_finalize(void);

static struct task g_tasks[TASK_MAX];
static uint32_t     g_next_id      = 1;
static uint32_t     g_kernel_cr3;
static uint32_t     g_init_pid     = 0;     /* set after init.elf is spawned */

/* SMP scheduler lock. Protects g_tasks[], the t->next ring, every
 * t->state transition, and cpu_local()->current writes. Held across
 * task_switch — see schedule()'s commentary for why that's correct
 * (and why it's the only design that doesn't race the prev->esp
 * save against another CPU dispatching prev). */
static spinlock_t g_sched_lock = SPINLOCK_INIT;

/* Convenience accessor — resolves the calling CPU's "current task"
 * via cpu_local(). Falls back to g_tasks[0] before SMP is up so
 * task.c works during the early-boot single-threaded window.
 *
 * IMPORTANT: cpu_local() reads LAPIC MMIO. If we're called BEFORE
 * lapic_init mapped the page, that's a fault. The g_smp_ready flag
 * (set by smp_init after lapic_init runs) gates which path we take.
 * Pre-SMP, we just return g_tasks[0] (the BSP idle / kmain). */
/* Non-static so tss.c can read it — same lazy dispatch trick. */
volatile int g_smp_ready;
void task_smp_ready(void) { g_smp_ready = 1; }

void post_switch_finalize(void) {
    /* Counterpart to schedule()'s spin_lock(&g_sched_lock). The
     * lock is held across task_switch as a hand-off; whichever code
     * path the new context lands in (this trampoline for new tasks,
     * fork_child_return for fork children, the spin_unlock after
     * task_switch for resumed tasks) is responsible for dropping it.
     * If it's already 0, spin_unlock is a no-op other than restoring
     * IF — harmless. */
    spin_unlock(&g_sched_lock);
}
static inline struct task *cpu_current(void) {
    if (!g_smp_ready) return &g_tasks[0];
    struct cpu_local *c = cpu_local();
    return c->current ? c->current : &g_tasks[0];
}

/* Forward decl — defined further down. */
static void user_entry_stub(void);

static inline uint32_t read_cr3(void) {
    uint32_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint32_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

void task_init(void) {
    memset(g_tasks, 0, sizeof(g_tasks));

    g_kernel_cr3 = read_cr3();

    /* Slot 0 is the bootstrapping thread (kmain). Its register state
     * is already live; we only need the TCB fields populated so the
     * first task_switch can save into it.
     *
     * NOT is_idle. We want BSP's kmain to participate in the regular
     * round-robin so the boot sequence (which runs as kmain) gets
     * preempted out and back in cleanly. cpu_pin = 0 ensures only the
     * BSP picks it — APs would crash trying to use BSP's boot stack. */
    g_tasks[0].id          = 0;
    g_tasks[0].state       = TASK_STATE_RUNNING;
    g_tasks[0].next        = &g_tasks[0];
    g_tasks[0].switches_in = 1;
    g_tasks[0].cr3         = g_kernel_cr3;
    g_tasks[0].cwd_dir     = TASK_CWD_ROOT;
    g_tasks[0].cpu         = 0;        /* BSP is logical CPU 0 */
    g_tasks[0].cpu_pin     = 0;        /* BSP-only: shares boot stack */
    g_tasks[0].is_idle     = 0;
    strncpy(g_tasks[0].name, "kmain", TASK_NAME_MAX - 1);

    /* Bind to BSP's per-CPU current pointer via cpu_at(0). We can't
     * use cpu_local() here — task_init runs BEFORE smp_init and the
     * LAPIC MMIO isn't mapped yet, so reading the LAPIC ID register
     * would page-fault. cpu_at(0) is a plain array index with no
     * MMIO access. After smp_init runs, task_smp_ready() flips the
     * flag and cpu_local() becomes safe to use. */
    struct cpu_local *bsp = cpu_at(0);
    if (bsp) bsp->current = &g_tasks[0];
}

/* Allocate a per-CPU idle TCB for the given CPU (other than the BSP,
 * whose idle is g_tasks[0] above). The caller is the AP itself,
 * running on its initial trampoline-provided kernel stack — that
 * stack BECOMES the idle task's stack. The CPU's `idle_stack`
 * (allocated in smp_init) IS the kernel stack we're standing on,
 * so we use it as the TCB's stack_base too. */
struct task *task_init_ap_idle(uint32_t cpu_id, uint32_t kernel_stack_top,
                               void *kernel_stack_base) {
    /* Find a free slot. AP idles permanently occupy a slot — they're
     * never reaped. */
    int slot = -1;
    spin_lock(&g_sched_lock);
    for (int i = 1; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_STATE_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock(&g_sched_lock);
        return NULL;
    }

    struct task *t = &g_tasks[slot];
    memset(t, 0, sizeof(*t));
    t->id               = 0;             /* idle tasks aren't pids the user sees */
    t->state            = TASK_STATE_RUNNING;  /* AP starts running idle  */
    t->cpu              = (int)cpu_id;
    t->cpu_pin          = (int)cpu_id;   /* AP idle is pinned to its CPU */
    t->is_idle          = 1;
    t->cr3              = g_kernel_cr3;
    t->kernel_stack_top = kernel_stack_top;
    t->stack_base       = kernel_stack_base;
    t->cwd_dir          = TASK_CWD_ROOT;
    t->switches_in      = 1;
    /* Ringless: idle tasks aren't on the round-robin chain. They're
     * picked only when the chain has nothing READY for this CPU. */
    t->next             = NULL;
    t->name[0] = 'i'; t->name[1] = 'd'; t->name[2] = 'l'; t->name[3] = 'e';
    t->name[4] = (char)('0' + cpu_id); t->name[5] = 0;

    spin_unlock(&g_sched_lock);
    return t;
}

struct task *task_create(task_fn entry, const char *name) {
    /* Find a free slot. */
    spin_lock(&g_sched_lock);
    int idx = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_STATE_UNUSED ||
            g_tasks[i].state == TASK_STATE_DEAD) {
            idx = i;
            break;
        }
    }
    if (idx < 0) { spin_unlock(&g_sched_lock); return NULL; }

    /* Reserve the slot before dropping the lock so a concurrent
     * task_create on another CPU can't pick the same one. Mark
     * it BLOCKED rather than READY: pick_next_ready scans for
     * READY only, so the half-initialized task is invisible to the
     * scheduler until our caller flips state to READY at the end
     * of its setup (see splice_into_ring below). */
    g_tasks[idx].state = TASK_STATE_BLOCKED;
    g_tasks[idx].next  = NULL;
    spin_unlock(&g_sched_lock);

    void *stack = kmalloc(TASK_STACK_SZ);
    if (!stack) {
        spin_lock(&g_sched_lock);
        g_tasks[idx].state = TASK_STATE_UNUSED;
        spin_unlock(&g_sched_lock);
        return NULL;
    }

    struct task *t = &g_tasks[idx];
    /* Don't memset — that'd zero the state we just claimed. Hand-init
     * each field instead so the slot stays "claimed" throughout. */
    int reserved_state = t->state;
    memset(t, 0, sizeof(*t));
    t->state      = reserved_state;
    t->id         = g_next_id++;
    t->cpu        = -1;
    t->cpu_pin    = -1;             /* any CPU may run this task */
    t->stack_base = stack;
    if (name) {
        strncpy(t->name, name, TASK_NAME_MAX - 1);
        t->name[TASK_NAME_MAX - 1] = 0;
    }

    /* Synthesize the initial stack frame. task_switch's pop sequence
     * will pull these values out:
     *
     *   higher addr ─┬─────────────────────────────────────┐
     *                │ 0          (fake return EIP)        │  <- entry must not return
     *                │ entry      (popped by trampoline's  │
     *                │             `ret`; jumps to real    │
     *                │             entry function)         │
     *                │ trampoline (popped by task_switch's │
     *                │             `ret`; releases the     │
     *                │             scheduler lock first)   │
     *                │ EFLAGS (IF set)                     │  <- popfl
     *                │ EBP                                  │
     *                │ EBX                                  │
     *                │ ESI                                  │
     *                │ EDI                                  │  <- task->esp
     *   lower addr  ─┴─────────────────────────────────────┘
     *
     * The extra trampoline indirection is what makes SMP scheduling
     * work: a freshly-scheduled new task can't fall through to the
     * spin_unlock() at the end of schedule() because it's never been
     * inside schedule() — it boots straight into `entry`. Without
     * the trampoline releasing the lock, the very next schedule
     * anywhere in the system would spin forever.
     */
    uintptr_t  top = (uintptr_t)stack + TASK_STACK_SZ;
    uint32_t  *sp  = (uint32_t *)top;

    *--sp = 0;                                          /* if entry returns -> #PF on 0 */
    *--sp = (uint32_t)(uintptr_t)entry;                 /* trampoline's ret target      */
    *--sp = (uint32_t)(uintptr_t)&task_entry_trampoline;/* task_switch ret target       */
    *--sp = 0x202;                                      /* EFLAGS: reserved bit + IF=1  */
    *--sp = 0;                                          /* EBP                           */
    *--sp = 0;                                          /* EBX                           */
    *--sp = 0;                                          /* ESI                           */
    *--sp = 0;                                          /* EDI                           */

    t->esp = (uint32_t)(uintptr_t)sp;
    t->cr3 = g_kernel_cr3;
    t->kernel_stack_top = (uint32_t)top;
    t->is_user = 0;

    /* Pre-wire fd 0/1/2 to stdin/stdout/stderr. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        t->fds[i].kind    = FD_FREE;
        t->fds[i].obj_idx = -1;
        t->fds[i].offset  = 0;
    }
    t->fds[0].kind = FD_STDIN;
    t->fds[1].kind = FD_STDOUT;
    t->fds[2].kind = FD_STDOUT;

    /* Signal table: all SIG_DFL, no pending, no mask, no trampoline yet. */
    signal_init_task(t);

    /* Heap: empty until the user calls SYS_BRK. */
    t->heap_brk = USER_HEAP_START;

    /* mmap (session 24): no regions, bump-allocator at the window
     * start. Each sys_mmap call advances mmap_brk; pages are demand-
     * loaded by the page fault handler. */
    for (int i = 0; i < TASK_MMAP_MAX; i++) t->mmaps[i].in_use = 0;
    t->mmap_brk = USER_MMAP_START;

    /* cwd defaults to / (session 25). */
    t->cwd_dir = TASK_CWD_ROOT;

    /* Splice into the round-robin list right after current. State
     * stays BLOCKED — caller is responsible for the final READY
     * transition once any post-task_create field setup is done.
     * For plain kernel-task callers (demo_a, reaper) that means
     * one extra task_make_runnable() call after task_create. For
     * task_create_user / task_fork the customization happens
     * before they call task_make_runnable. */
    spin_lock(&g_sched_lock);
    struct task *cur = cpu_current();
    t->next = cur->next ? cur->next : cur;
    cur->next = t;
    spin_unlock(&g_sched_lock);

    return t;
}

/* Caller-side promotion: flip the task from BLOCKED (the state
 * task_create leaves it in) to READY so the scheduler can dispatch
 * it. Held only across the state write, then released — quick. */
void task_make_runnable(struct task *t) {
    if (!t) return;
    spin_lock(&g_sched_lock);
    t->state = TASK_STATE_READY;
    spin_unlock(&g_sched_lock);
}

/*
 * Build a ring-3 task. The first time it's scheduled, task_switch's
 * `ret` jumps to user_entry_stub (running in ring 0 on the task's
 * kernel stack). user_entry_stub configures the TSS, builds an iret
 * frame for ring 3, and irets into user code.
 *
 * Race with the SMP scheduler (session 38): task_create returns the
 * task in TASK_STATE_READY, which means an AP could pick it up
 * BEFORE we set cr3 / user_eip / user_esp here. The task would
 * iret to whatever uninitialized garbage is in the user_eip slot.
 * Fix: temporarily demote to BLOCKED while we customize, then
 * promote back to READY at the end. Both transitions are protected
 * by g_sched_lock.
 */
struct task *task_create_user(uint32_t user_eip, uint32_t user_esp,
                              uint32_t cr3, const char *name) {
    /* task_create leaves the new task in TASK_STATE_BLOCKED so it's
     * invisible to pick_next_ready while we finish field setup
     * here. The final task_make_runnable() flips state→READY. */
    struct task *t = task_create(user_entry_stub, name);
    if (!t) return NULL;
    t->cr3      = cr3;
    t->user_eip = user_eip;
    t->user_esp = user_esp;
    t->is_user  = 1;
    /* Session 38: BKL + BLOCKED-create + reaper/syncer locking laid
     * the groundwork. Migration to AP is GATED at boot via the
     * AP_CAN_RUN_USER kconfig (kernel/kernel.c). Default OFF until
     * cross-CPU TLB shootdowns are added — without those, occasional
     * stale-PTE faults manifest in long-running fork/exec storms.
     * Set the flag to 1 to opt in and observe; see deep dive 38. */
    extern int g_ap_runs_user;
    t->cpu_pin = g_ap_runs_user ? -1 : 0;
    task_make_runnable(t);
    return t;
}

/*
 * Ring-0 trampoline that takes a freshly-scheduled user task into
 * ring 3. Runs once per task at first scheduling; subsequent
 * preemptions resume the task via the normal ISR-iret path instead.
 */
static void user_entry_stub(void) {
    struct task *t = cpu_current();
    tss_set_kernel_stack(t->kernel_stack_top);

    /* Selectors with RPL=3:  0x18|3 = 0x1B (user code), 0x20|3 = 0x23 (user data).
     * Pin operands to ESI/EDI ("S"/"D") so the segment-load sequence
     * below can't accidentally clobber an operand register the
     * compiler chose for us. */
    __asm__ volatile (
        "mov $0x23, %%ax     \n\t"
        "mov %%ax, %%ds      \n\t"
        "mov %%ax, %%es      \n\t"
        "mov %%ax, %%fs      \n\t"
        "mov %%ax, %%gs      \n\t"
        "push $0x23          \n\t"   /* SS                            */
        "push %0             \n\t"   /* user ESP (esi)                */
        "push $0x202         \n\t"   /* EFLAGS: reserved bit + IF=1   */
        "push $0x1B          \n\t"   /* CS                            */
        "push %1             \n\t"   /* user EIP (edi)                */
        "iret                \n\t"
        :
        : "S"(t->user_esp), "D"(t->user_eip)
        : "eax", "memory"
    );
    __builtin_unreachable();
}

/* Tasks pinned to a different CPU can't be picked by us. */
static inline int pickable_by(const struct task *t, int my_cpu) {
    if (t->state != TASK_STATE_READY) return 0;
    if (t->cpu_pin != -1 && t->cpu_pin != my_cpu) return 0;
    return 1;
}

/* Walk the round-robin ring starting after `from`, return the first
 * task whose state == TASK_STATE_READY AND whose cpu_pin admits this
 * CPU. If nothing is found, returns NULL — caller falls back to the
 * per-CPU idle task.
 *
 * Caller MUST hold g_sched_lock — we read state and ring pointers
 * that other CPUs can mutate.
 *
 * Tasks marked TASK_STATE_RUNNING are skipped (already in flight).
 * Idle tasks (is_idle) aren't on the ring at all.
 *
 * `from` may be NULL (AP idle yielding) or any task in the ring. We
 * always start from `from->next` if available so different CPUs
 * make progress through different parts of the ring. */
static struct task *pick_next_ready(struct task *from, int my_cpu) {
    struct task *start = (from && from->next) ? from->next : NULL;
    if (!start) {
        /* No ring entry to anchor on (e.g., AP idle yielding). Scan
         * all slots for a runnable + pickable task. */
        for (int i = 0; i < TASK_MAX; i++) {
            if (pickable_by(&g_tasks[i], my_cpu)) return &g_tasks[i];
        }
        return NULL;
    }
    struct task *t = start;
    int safety = TASK_MAX * 2;
    do {
        if (pickable_by(t, my_cpu)) return t;
        t = t->next ? t->next : start;
        if (!--safety) break;
    } while (t != start);
    if (pickable_by(start, my_cpu)) return start;
    return NULL;
}

void schedule(void) {
    /* Take the global scheduler lock. spin_lock cli's first, so we're
     * race-free against IRQs on this CPU and against schedule() on
     * other CPUs. The lock is HELD across task_switch (see commentary
     * below) — we don't drop it until after the switch completes from
     * the perspective of the *incoming* task. */
    spin_lock(&g_sched_lock);

    /* cpu_local() reads LAPIC MMIO, which isn't mapped pre-SMP. Use
     * the array-index variant cpu_at(0) until smp_init has flipped
     * task_smp_ready. */
    struct cpu_local *cpu  = g_smp_ready ? cpu_local() : cpu_at(0);
    struct task      *prev = cpu->current ? cpu->current : &g_tasks[0];

    /* If prev is RUNNING (the normal case), demote to READY so other
     * CPUs are free to dispatch it. Idle tasks are special: they're
     * not on the ring and don't get demoted — they stay RUNNING on
     * "their" CPU and we just leave them alone if there's no work.
     *
     * BLOCKED / ZOMBIE / DEAD / STOPPED / BLOCKED_ON_CHILD: someone
     * else (sys_exit, mutex_lock, etc.) already set the right state;
     * we leave it alone and just yield. */
    if (prev->state == TASK_STATE_RUNNING && !prev->is_idle) {
        prev->state = TASK_STATE_READY;
        prev->cpu   = -1;
    }

    struct task *next = pick_next_ready(prev->is_idle ? NULL : prev,
                                        (int)cpu->cpu_id);

    /* Fall back to this CPU's idle task if no real work is available.
     * The BSP's idle is g_tasks[0]; APs install their own via
     * task_init_ap_idle. cpu->idle == NULL would mean a setup bug. */
    if (!next) {
        next = (struct task *)cpu->idle;
        if (!next) next = &g_tasks[0];   /* last-ditch fallback */
    }

    if (next == prev) {
        /* Nothing to do — drop lock and return. The popfl in
         * spin_unlock restores IF if prev had it set at acquire. */
        spin_unlock(&g_sched_lock);
        return;
    }

    next->state       = TASK_STATE_RUNNING;
    next->cpu         = (int)cpu->cpu_id;
    next->switches_in++;
    cpu->current      = next;

    /* Diagnostic counter: how many times each CPU has dispatched a
     * non-idle task. Read via SYS_SMP_STATS / shown in [t22]. */
    extern volatile uint32_t g_cpu_dispatch[8];
    if (!next->is_idle && cpu->cpu_id < 8) g_cpu_dispatch[cpu->cpu_id]++;

    /* Per-CPU TSS.esp0 update so any ring-3 -> ring-0 transition that
     * happens AFTER this switch (syscall, page fault, IRQ) lands on
     * the incoming task's kernel stack. tss_set_kernel_stack reads
     * cpu_local() to find the right TSS. */
    if (next->kernel_stack_top) {
        tss_set_kernel_stack(next->kernel_stack_top);
    }

    /* Switch address space if needed. Kernel mappings are mirrored
     * across every PD so the kernel stack we're standing on stays
     * reachable across the CR3 write. */
    if (next->cr3 && next->cr3 != prev->cr3) {
        write_cr3(next->cr3);
    }

    /* Hand-off the lock across task_switch. The lock stays HELD; when
     * the incoming task (`next`) eventually returns from its own past
     * task_switch call site below, IT will spin_unlock. This is the
     * piece that makes the whole "set prev->state = READY before
     * task_switch" pattern safe — no other CPU can inspect prev or
     * dispatch it while we still own the lock, so prev->esp is
     * guaranteed to be saved by task_switch.S before any CPU resumes
     * prev. (See docs/33-ap-scheduling.md §"the lock-handoff trick".) */
    task_switch(&prev->esp, next->esp);

    /* Resumed here LATER, on whichever CPU now runs prev. The lock
     * is still owned (at the level of the global flag); release it
     * to let the rest of the kernel run normally on this CPU. */
    spin_unlock(&g_sched_lock);
}

void task_yield(void) {
    /* If we're inside a syscall (BKL held by us), drop the BKL
     * across the schedule. Without this, other CPUs would
     * eventually deadlock waiting on a BKL that's held by a
     * task that's not currently running. The trade-off: other
     * CPUs can enter the kernel during our yield, so any in-
     * progress kernel state we left behind must be tolerable
     * to concurrent access (pmm/kmalloc/sched have their own
     * locks; fs/bcache/elf assume serial access — we mostly
     * don't yield from inside those). */
    extern int  bkl_held(void);
    extern void bkl_lock(void);
    extern void bkl_unlock(void);
    if (bkl_held()) {
        bkl_unlock();
        schedule();
        bkl_lock();
    } else {
        schedule();
    }
}

struct task *task_current(void) {
    return cpu_current();
}

struct task *task_at(uint32_t slot) {
    if (slot >= TASK_MAX) return NULL;
    if (g_tasks[slot].state == TASK_STATE_UNUSED) return NULL;
    return &g_tasks[slot];
}

const char *task_state_name(int s) {
    switch (s) {
        case TASK_STATE_READY:            return "READY";
        case TASK_STATE_RUNNING:          return "RUN";
        case TASK_STATE_BLOCKED:          return "BLOCK";
        case TASK_STATE_DEAD:             return "DEAD";
        case TASK_STATE_BLOCKED_ON_CHILD: return "WAIT";
        case TASK_STATE_ZOMBIE:           return "ZOMB";
        case TASK_STATE_STOPPED:          return "STOP";
        default:                          return "?";
    }
}

/*
 * Reaper. Walks the task table looking for DEAD slots, unlinks them
 * from the round-robin ring, frees their resources, and marks the
 * slot UNUSED so task_create can reuse it. Sleeps between sweeps
 * so it doesn't burn CPU when there's nothing to do.
 *
 * Safety:
 *   - We never reap the current task (you can't pull the rug out
 *     from under yourself).
 *   - The list mutation runs with cli to be atomic vs the scheduler.
 *   - The PMM/kmalloc frees themselves take their own spinlocks,
 *     so we drop cli before those calls.
 */
static void task_reaper(void) {
    extern void bkl_lock(void);
    extern void bkl_unlock(void);
    for (;;) {
        pit_sleep(200);

        /* Reaper touches paging_destroy_user_pd (alloc/free pages),
         * the FS-resident exit-code field, and the g_tasks ring.
         * Hold BKL so it serializes with user-side syscalls. */
        bkl_lock();
        for (int i = 1; i < TASK_MAX; i++) {
            struct task *t = &g_tasks[i];
            /* Skip the per-CPU current of any CPU — pulling the rug
             * out from under a running task is fatal. The is_idle
             * check is for the BSP idle (g_tasks[0]) which never
             * goes DEAD anyway but is a defensive belt-and-suspenders. */
            if (t->is_idle)                  continue;
            if (t->state != TASK_STATE_DEAD) continue;
            if (t->stack_base == NULL)       continue;  /* already reaped */

            int still_running = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                struct cpu_local *cl = cpu_at(c);
                if (cl && cl->current == t) { still_running = 1; break; }
            }
            if (still_running) continue;

            /* Splice the dead task out of the round-robin ring under
             * the scheduler lock — other CPUs may be walking it via
             * pick_next_ready right now. */
            spin_lock(&g_sched_lock);
            struct task *p = t->next;
            int safety = TASK_MAX * 2;
            while (p && p != t && p->next != t && safety--) p = p->next;
            if (p && p->next == t) p->next = t->next;
            spin_unlock(&g_sched_lock);

            uint32_t reaped_id   = t->id;
            int      reaped_user = t->is_user;
            uint32_t reaped_cr3  = t->cr3;
            void    *reaped_stk  = t->stack_base;

            /* Mark the slot UNUSED first; this stops anyone else
             * (e.g. shell `tasks`) from observing a half-torn-down TCB. */
            t->state            = TASK_STATE_UNUSED;
            t->stack_base       = NULL;
            t->cr3              = 0;
            t->kernel_stack_top = 0;
            t->user_eip         = 0;
            t->user_esp         = 0;
            t->is_user          = 0;
            t->next             = NULL;
            t->wait_next        = NULL;

            /* Now free the heavy stuff outside the cli section. */
            if (reaped_user && reaped_cr3 && reaped_cr3 != g_kernel_cr3) {
                paging_destroy_user_pd((uint32_t *)(uintptr_t)reaped_cr3);
            }
            kfree(reaped_stk);

            kprintf("[reaper] freed pid=%u (%s task), slot %d now UNUSED\n",
                    (unsigned)reaped_id,
                    reaped_user ? "user" : "kernel",
                    i);
        }
        bkl_unlock();
    }
}

void task_reaper_start(void) {
    task_make_runnable(task_create(task_reaper, "reaper"));
}

void task_set_init_pid(uint32_t pid) { g_init_pid = pid; }
uint32_t task_get_init_pid(void)     { return g_init_pid; }

/* -------------------------------------------------------------------- */
/* fork / exec / wait                                                   */
/* -------------------------------------------------------------------- */

/* Find a free TCB slot, exactly like task_create's first loop. */
static int find_free_slot(void) {
    for (int i = 1; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_STATE_UNUSED ||
            g_tasks[i].state == TASK_STATE_DEAD) {
            return i;
        }
    }
    return -1;
}

/*
 * Build the child's kernel-stack frame for fork. See docs/14-fork-exec-wait.md
 * for the diagram. The high half of the stack is a copy of the parent's
 * isr_common_stub frame (with EAX = 0 in the popa block); the low half is
 * the task_switch frame whose `ret` jumps to fork_child_return.
 */
static uint32_t synth_fork_child_stack(void *stack_base,
                                       const struct registers *parent) {
    uint8_t *top = (uint8_t *)stack_base + TASK_STACK_SZ;

    /* iret frame (5 dwords). */
    top -= 4; *(uint32_t *)top = parent->ss;
    top -= 4; *(uint32_t *)top = parent->useresp;
    top -= 4; *(uint32_t *)top = parent->eflags;
    top -= 4; *(uint32_t *)top = parent->cs;
    top -= 4; *(uint32_t *)top = parent->eip;

    /* err_code, int_no — skipped by `add $8, %esp` in fork_child_return. */
    top -= 4; *(uint32_t *)top = parent->err_code;
    top -= 4; *(uint32_t *)top = parent->int_no;

    /* popa frame (8 dwords, in pop order: eax first off, edi last off).
     * Stored low-to-high here so the actual order on the stack mirrors
     * what `pusha` produced in the parent: edi @ lowest, eax @ highest. */
    top -= 4; *(uint32_t *)top = 0;                     /* eax = 0 — fork() == 0 */
    top -= 4; *(uint32_t *)top = parent->ecx;
    top -= 4; *(uint32_t *)top = parent->edx;
    top -= 4; *(uint32_t *)top = parent->ebx;
    top -= 4; *(uint32_t *)top = parent->esp_orig;      /* popa ignores this slot */
    top -= 4; *(uint32_t *)top = parent->ebp;
    top -= 4; *(uint32_t *)top = parent->esi;
    top -= 4; *(uint32_t *)top = parent->edi;

    /* saved DS — pop %eax in fork_child_return then mov to ds/es/fs/gs. */
    top -= 4; *(uint32_t *)top = parent->ds;

    /* task_switch's `ret` will pop this as the return address. */
    top -= 4; *(uint32_t *)top = (uint32_t)(uintptr_t)&fork_child_return;

    /* task_switch's pop sequence: EFLAGS (popfl), then EBP/EBX/ESI/EDI.
     * EFLAGS = 0x202 → reserved bit set + IF=1, like task_create. */
    top -= 4; *(uint32_t *)top = 0x202;
    top -= 4; *(uint32_t *)top = 0;                     /* EBP */
    top -= 4; *(uint32_t *)top = 0;                     /* EBX */
    top -= 4; *(uint32_t *)top = 0;                     /* ESI */
    top -= 4; *(uint32_t *)top = 0;                     /* EDI */

    return (uint32_t)(uintptr_t)top;                    /* child's saved esp */
}

struct task *task_fork(struct registers *parent_regs) {
    struct task *parent = cpu_current();
    if (!parent->is_user) return NULL;                  /* only ring-3 tasks fork */

    /* 1. New TCB + kernel stack. */
    int slot = find_free_slot();
    if (slot < 0) return NULL;

    void *kstack = kmalloc(TASK_STACK_SZ);
    if (!kstack) return NULL;

    /* 2. Deep-copy parent's user PD. */
    uint32_t *child_pd = paging_clone_user_pd((uint32_t *)(uintptr_t)parent->cr3);
    if (!child_pd) {
        kfree(kstack);
        return NULL;
    }

    /* 3. Synthesize child kernel stack so its first scheduling lands
     *    in fork_child_return with EAX=0 and the parent's iret frame. */
    uint32_t child_esp = synth_fork_child_stack(kstack, parent_regs);

    /* 4. Populate the TCB. We do this before splicing into the round-
     *    robin ring so the scheduler never sees a half-built task.
     *
     * SMP race (session 38): pick_next_ready scans all g_tasks for
     * READY when no ring anchor is given (AP idle's case). If we
     * set state=READY before all the inheritance below is done, an
     * AP could dispatch the child with stale fds, signals, etc.
     * Hide it as BLOCKED while we set up; transition to READY at
     * the very end (after the splice). */
    struct task *child = &g_tasks[slot];
    memset(child, 0, sizeof(*child));
    child->id               = g_next_id++;
    child->state            = TASK_STATE_BLOCKED;
    child->cpu              = -1;
    /* Session 38: same gate as task_create_user — flag controls
     * whether children migrate to AP. Off by default. */
    extern int g_ap_runs_user;
    child->cpu_pin          = g_ap_runs_user ? -1 : 0;
    child->stack_base       = kstack;
    child->esp              = child_esp;
    child->cr3              = (uint32_t)(uintptr_t)child_pd;
    child->kernel_stack_top = (uint32_t)(uintptr_t)kstack + TASK_STACK_SZ;
    child->user_eip         = parent_regs->eip;
    child->user_esp         = parent_regs->useresp;
    child->is_user          = 1;
    child->parent_id        = parent->id;
    child->exit_code        = 0;
    strncpy(child->name, parent->name, TASK_NAME_MAX - 1);

    /* 5. Inherit fd table. memcpy the entries + bump refcounts on the
     *    underlying objects for kinds that reference-count (pipes,
     *    tmpfs). Sockets and FS handles share by index without
     *    explicit refs — fine today because the shell doesn't fork
     *    while sockets are open and FS handles are read-only. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        child->fds[i] = parent->fds[i];
        switch (parent->fds[i].kind) {
            case FD_PIPE_R: pipe_inc_read (parent->fds[i].obj_idx); break;
            case FD_PIPE_W: pipe_inc_write(parent->fds[i].obj_idx); break;
            case FD_TMPFS:  tmpfs_inc_ref (parent->fds[i].obj_idx); break;
            case FD_SOCK:   sock_inc_ref  (parent->fds[i].obj_idx); break;
            default: break;
        }
    }

    /* 5b. Inherit signal handler table + mask + trampoline. POSIX
     *     spec: pending set is NOT inherited (the child starts with
     *     no pending signals). signal_init_task already zeroed all
     *     of the child's signal state in its memset path; we now
     *     overlay the handlers + mask + trampoline from the parent. */
    for (int i = 0; i < NSIG; i++) child->sig_handlers[i] = parent->sig_handlers[i];
    child->sig_mask  = parent->sig_mask;
    child->sig_tramp = parent->sig_tramp;
    child->sig_pending = 0;

    /* 5c. Inherit heap_brk verbatim. The deep-PD copy already
     *     duplicated the heap pages — child has its own physical
     *     copies at the same VAs, so its heap_brk is meaningful. */
    child->heap_brk = parent->heap_brk;

    /* 5e. Inherit mmap regions verbatim (session 24). Pages already
     *     faulted in for the parent are deep-copied with the rest of
     *     the user PD. Pages NOT yet faulted will be faulted-and-
     *     populated independently in each task. */
    for (int i = 0; i < TASK_MMAP_MAX; i++) child->mmaps[i] = parent->mmaps[i];
    child->mmap_brk = parent->mmap_brk;

    /* 5f. Inherit cwd (session 25). POSIX. */
    child->cwd_dir = parent->cwd_dir;

    /* 5d. Inherit job-control state (session 20). POSIX: fork
     *     preserves both pgid and sid. setpgid is used by the
     *     parent (or child) to put the child into its own pgrp. */
    child->pgid = parent->pgid;
    child->sid  = parent->sid;

    /* 5e. Inherit process credentials (session 47). uid/gid
     *     survive fork verbatim; only an explicit SYS_SETUID
     *     (from a uid-0 caller — login.elf is the canonical one)
     *     changes them. */
    child->uid = parent->uid;
    child->gid = parent->gid;

    /* 6. Splice into the round-robin ring under the scheduler lock —
     * APs may be picking from the ring concurrently. Transition
     * state to READY in the same critical section so the moment
     * the splice is visible the task is also dispatchable. */
    spin_lock(&g_sched_lock);
    child->next       = parent->next;
    parent->next      = child;
    child->state      = TASK_STATE_READY;
    spin_unlock(&g_sched_lock);

    return child;
}

int task_exec_inplace(struct registers *r,
                      const char *path,
                      int argc,
                      const char *const *argv_strs) {
    struct task *t = cpu_current();
    if (!t->is_user) return -1;

    /* Open + load the new ELF FIRST, into a fresh PD. If anything
     * fails we abort with the caller's address space untouched. */
    int fd = fs_open(path);
    if (fd < 0) return -2;
    /* Session 48: require execute permission. Without this any user
     * could turn an arbitrary readable file into a running program
     * by exec'ing it. fs_check_perm returns 1 = allowed (incl. root
     * bypass), 0 = denied, -1 = bad index. */
    if (fs_check_perm(fd, FS_PERM_X) != 1) return -3;

    struct elf_load_result lr;
    int err = elf_load(fd, &lr);
    if (err != 0) return err;

    /* Pack argv onto the new stack. elf_setup_args writes via the
     * kernel identity map (lr.stack_phys), so the new PD doesn't
     * need to be active yet. */
    elf_setup_args(&lr, argc, argv_strs);

    /* Commit: switch CR3, free the old PD, rewrite the syscall return
     * frame so iret jumps to the new program. From here on we're past
     * the no-return point — failure means the task has no usable
     * address space. */
    uint32_t old_cr3 = t->cr3;
    t->cr3      = lr.cr3;
    t->user_eip = lr.entry;
    t->user_esp = lr.user_esp;
    if (path) {
        strncpy(t->name, path, TASK_NAME_MAX - 1);
        t->name[TASK_NAME_MAX - 1] = 0;
    }

    /* Activate the new address space. The kernel stack we're standing
     * on is in the 0..32 MiB identity-mapped region, mirrored into
     * every user PD via shared kernel PDEs, so the load doesn't pull
     * the rug out. */
    __asm__ volatile ("mov %0, %%cr3" :: "r"(lr.cr3) : "memory");

    /* Now safe to free the old PD — nothing references its mappings. */
    paging_destroy_user_pd((uint32_t *)(uintptr_t)old_cr3);

    /* Reset signal handlers per POSIX exec semantics: SIG_DFL takes
     * over for any caught signal; SIG_IGN survives. The trampoline
     * VA is in the OLD address space and the new ELF will install
     * its own when it calls signal() — clear it. */
    signal_reset_on_exec(t);

    /* Reset the heap. The old PD (and all heap pages) is gone; the
     * new ELF gets a fresh empty heap that will grow on its first
     * malloc → sys_brk. */
    t->heap_brk = USER_HEAP_START;

    /* Reset mmap regions (session 24). The old PD's mappings are
     * gone with paging_destroy_user_pd; the new ELF gets a clean
     * mmap window. */
    for (int i = 0; i < TASK_MMAP_MAX; i++) t->mmaps[i].in_use = 0;
    t->mmap_brk = USER_MMAP_START;

    /* Rewrite the iret frame so the syscall return jumps into the
     * freshly-loaded program at its entry point with the new ESP. */
    r->eip     = lr.entry;
    r->useresp = lr.user_esp;
    r->cs      = 0x1B;        /* user code, RPL=3                     */
    r->ss      = 0x23;        /* user data, RPL=3                     */
    r->ds      = 0x23;        /* restored by isr_common_stub tail     */
    r->eflags  = 0x202;       /* IF=1, reserved bit set               */
    r->eax     = 0;           /* _start ignores eax, but be tidy      */

    return 0;
}

/* Drop all per-fd references this task holds. Mirrors syscall.c's
 * release_fd() but inlined here so task.c doesn't need to include
 * syscall.h. Kernel-only tasks (no user PD) skip this — they don't
 * own user-visible fds. */
static void close_all_fds(struct task *t) {
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        struct task_fd *e = &t->fds[i];
        switch (e->kind) {
            case FD_SOCK:    sock_close       (e->obj_idx); break;
            case FD_PIPE_R:  pipe_close_read  (e->obj_idx); break;
            case FD_PIPE_W:  pipe_close_write (e->obj_idx); break;
            case FD_TMPFS:   tmpfs_close      (e->obj_idx); break;
            default: break;
        }
        e->kind    = FD_FREE;
        e->obj_idx = -1;
        e->offset  = 0;
    }
}

void task_exit_current(int exit_code) {
    struct task *t = cpu_current();
    t->exit_code = exit_code;

    /* Close everything we still have open. Critical for pipes:
     * without this, a child that exits without explicitly close()'ing
     * its pipe-write end would leave write_refs > 0 forever and the
     * reader on the other side would never see EOF. */
    if (t->is_user) close_all_fds(t);

    /* Reparent any of our surviving children to init (session 22).
     * Without this, an orphan goes to DEAD and the kernel reaper
     * frees it, but the user-side semantics of "init reaps everyone"
     * don't work — there's no parent to wake. With init holding
     * pid g_init_pid, an orphan becomes init's child and shows up in
     * init's normal sys_wait loop. We skip if we ARE init (init
     * dying is its own emergency we don't model). */
    if (g_init_pid != 0 && t->id != g_init_pid) {
        for (int i = 0; i < TASK_MAX; i++) {
            if (g_tasks[i].state == TASK_STATE_UNUSED) continue;
            if (g_tasks[i].parent_id == t->id) {
                g_tasks[i].parent_id = g_init_pid;
                /* If the child has already exited (zombie), and init
                 * is currently blocked waiting, wake it so the new
                 * adoption doesn't sit forever. */
                if (g_tasks[i].state == TASK_STATE_ZOMBIE) {
                    for (int j = 0; j < TASK_MAX; j++) {
                        if (g_tasks[j].id == g_init_pid &&
                            g_tasks[j].state == TASK_STATE_BLOCKED_ON_CHILD) {
                            g_tasks[j].state = TASK_STATE_READY;
                        }
                    }
                }
            }
        }
    }

    /* If our parent is waiting for any child, wake it. The parent will
     * loop scanning its children for a zombie and harvest us. */
    struct task *parent = NULL;
    if (t->parent_id != 0) {
        for (int i = 0; i < TASK_MAX; i++) {
            if (g_tasks[i].id == t->parent_id &&
                g_tasks[i].state != TASK_STATE_UNUSED) {
                parent = &g_tasks[i];
                break;
            }
        }
    }

    if (parent && parent->state == TASK_STATE_BLOCKED_ON_CHILD) {
        parent->state = TASK_STATE_READY;
    }

    /* If we have a live parent, we become a zombie waiting to be
     * harvested by sys_wait; otherwise we go straight to DEAD and the
     * reaper frees us. */
    if (parent) {
        t->state = TASK_STATE_ZOMBIE;
    } else {
        t->state = TASK_STATE_DEAD;
    }
}

/* Look for any zombie child of `parent`. If found, harvest its exit
 * code, free the child's user PD + kernel stack, splice it out of the
 * round-robin ring, mark its slot UNUSED, and return its pid. Returns
 * 0 if no zombie child is currently sitting in the table.
 *
 * The cleanup happens synchronously here (not deferred to the reaper)
 * because the slot needs to be reusable before the parent's next
 * fork() — otherwise the next fork could reuse a still-spliced slot
 * and inherit `parent->next == new_child_slot`, producing a ring
 * self-loop. The reaper still handles orphan tasks (parent already
 * gone) which go DEAD instead of ZOMBIE. */
static uint32_t reap_one_zombie_of(struct task *parent, int *out_code) {
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *c = &g_tasks[i];
        if (c->state    != TASK_STATE_ZOMBIE) continue;
        if (c->parent_id != parent->id)       continue;

        uint32_t pid = c->id;
        if (out_code) *out_code = c->exit_code;

        /* Unsplice from the round-robin ring atomically vs the
         * scheduler. Safe because c is ZOMBIE — it's not running and
         * won't be picked. */
        spin_lock(&g_sched_lock);
        struct task *p = c->next;
        int safety = TASK_MAX * 2;
        while (p && p != c && p->next != c && safety--) p = p->next;
        if (p && p->next == c) p->next = c->next;
        spin_unlock(&g_sched_lock);

        /* Free the child's address space + kernel stack. We're on the
         * parent's kernel stack; the parent's CR3 is loaded; the
         * zombie's CR3 is unloaded (no task currently references it
         * via the active CR3 register). */
        uint32_t cr3   = c->cr3;
        void    *stk   = c->stack_base;
        int      isusr = c->is_user;

        c->state            = TASK_STATE_UNUSED;
        c->stack_base       = NULL;
        c->cr3              = 0;
        c->kernel_stack_top = 0;
        c->user_eip         = 0;
        c->user_esp         = 0;
        c->is_user          = 0;
        c->next             = NULL;
        c->wait_next        = NULL;

        if (isusr && cr3 && cr3 != g_kernel_cr3) {
            paging_destroy_user_pd((uint32_t *)(uintptr_t)cr3);
        }
        if (stk) kfree(stk);

        return pid;
    }
    return 0;
}

/* Returns 1 if `parent` has any non-DEAD/UNUSED child. Used so wait()
 * can return -1 immediately when there's nothing to wait for. */
static int has_any_live_or_zombie_child(struct task *parent) {
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *c = &g_tasks[i];
        if (c->parent_id != parent->id) continue;
        if (c->state == TASK_STATE_UNUSED || c->state == TASK_STATE_DEAD) continue;
        return 1;
    }
    return 0;
}

int task_wait_current(int *out_code) {
    struct task *t = cpu_current();
    for (;;) {
        uint32_t pid = reap_one_zombie_of(t, out_code);
        if (pid != 0) return (int)pid;

        if (!has_any_live_or_zombie_child(t)) return -1;

        /* No zombie ready and at least one child still alive — sleep
         * until a child exits and flips us back to READY. */
        t->state = TASK_STATE_BLOCKED_ON_CHILD;
        schedule();
    }
}

int task_waitpid_nb_current(int *out_code) {
    struct task *t = cpu_current();
    uint32_t pid = reap_one_zombie_of(t, out_code);
    if (pid != 0) return (int)pid;
    if (!has_any_live_or_zombie_child(t)) return -1;
    return 0;       /* children alive, no zombie ready right now */
}
