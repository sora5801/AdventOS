#include "task.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "paging.h"
#include "pit.h"
#include "string.h"
#include "tss.h"
#include "../include/io.h"

extern void task_switch(uint32_t *old_esp, uint32_t new_esp);

static struct task g_tasks[TASK_MAX];
static struct task *g_current;
static uint32_t     g_next_id      = 1;
static uint32_t     g_kernel_cr3;

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
     * first task_switch can save into it. */
    g_tasks[0].id          = 0;
    g_tasks[0].state       = TASK_STATE_RUNNING;
    g_tasks[0].next        = &g_tasks[0];
    g_tasks[0].switches_in = 1;
    g_tasks[0].cr3         = g_kernel_cr3;
    strncpy(g_tasks[0].name, "kmain", TASK_NAME_MAX - 1);

    g_current = &g_tasks[0];
}

struct task *task_create(task_fn entry, const char *name) {
    /* Find a free slot. */
    int idx = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_STATE_UNUSED ||
            g_tasks[i].state == TASK_STATE_DEAD) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return NULL;

    void *stack = kmalloc(TASK_STACK_SZ);
    if (!stack) return NULL;

    struct task *t = &g_tasks[idx];
    memset(t, 0, sizeof(*t));
    t->id         = g_next_id++;
    t->state      = TASK_STATE_READY;
    t->stack_base = stack;
    if (name) {
        strncpy(t->name, name, TASK_NAME_MAX - 1);
        t->name[TASK_NAME_MAX - 1] = 0;
    }

    /* Synthesize the initial stack frame. task_switch's pop sequence
     * will pull these values out:
     *
     *   higher addr ─┬───────────────────────────┐
     *                │ 0      (fake return EIP)  │  <- entry must not return
     *                │ entry  (real return EIP)  │  <- popped by `ret`
     *                │ EFLAGS (IF set)           │  <- popfl
     *                │ EBP                       │
     *                │ EBX                       │
     *                │ ESI                       │
     *                │ EDI                       │  <- task->esp
     *   lower addr  ─┴───────────────────────────┘
     */
    uintptr_t  top = (uintptr_t)stack + TASK_STACK_SZ;
    uint32_t  *sp  = (uint32_t *)top;

    *--sp = 0;                          /* if entry returns -> #PF on 0 */
    *--sp = (uint32_t)(uintptr_t)entry; /* `ret` target                 */
    *--sp = 0x202;                      /* EFLAGS: reserved bit + IF=1  */
    *--sp = 0;                          /* EBP                           */
    *--sp = 0;                          /* EBX                           */
    *--sp = 0;                          /* ESI                           */
    *--sp = 0;                          /* EDI                           */

    t->esp = (uint32_t)(uintptr_t)sp;
    t->cr3 = g_kernel_cr3;
    t->kernel_stack_top = (uint32_t)top;
    t->is_user = 0;

    /* Splice into the round-robin list right after current. */
    __asm__ volatile ("cli");
    t->next = g_current->next;
    g_current->next = t;
    __asm__ volatile ("sti");

    return t;
}

/*
 * Build a ring-3 task. The first time it's scheduled, task_switch's
 * `ret` jumps to user_entry_stub (running in ring 0 on the task's
 * kernel stack). user_entry_stub configures the TSS, builds an iret
 * frame for ring 3, and irets into user code.
 */
struct task *task_create_user(uint32_t user_eip, uint32_t user_esp,
                              uint32_t cr3, const char *name) {
    struct task *t = task_create(user_entry_stub, name);
    if (!t) return NULL;
    t->cr3      = cr3;
    t->user_eip = user_eip;
    t->user_esp = user_esp;
    t->is_user  = 1;
    return t;
}

/*
 * Ring-0 trampoline that takes a freshly-scheduled user task into
 * ring 3. Runs once per task at first scheduling; subsequent
 * preemptions resume the task via the normal ISR-iret path instead.
 */
static void user_entry_stub(void) {
    struct task *t = g_current;
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

void schedule(void) {
    if (!g_current) return;

    __asm__ volatile ("cli");

    /* Pick the next runnable task in the circular list. Skip both
     * DEAD (gone forever) and BLOCKED (waiting on a sync primitive). */
    struct task *next = g_current->next;
    int safety = TASK_MAX;
    while (next != g_current
           && (next->state == TASK_STATE_DEAD ||
               next->state == TASK_STATE_BLOCKED)
           && safety--) {
        next = next->next;
    }

    if (next == g_current) {
        __asm__ volatile ("sti");
        return;
    }

    struct task *prev = g_current;
    if (prev->state == TASK_STATE_RUNNING) prev->state = TASK_STATE_READY;
    next->state = TASK_STATE_RUNNING;
    next->switches_in++;
    g_current = next;

    /* TSS.esp0 must point at the incoming task's kernel stack so any
     * future ring-3 -> ring-0 entry (syscall or interrupt) lands on
     * the right per-task kernel stack. */
    if (next->kernel_stack_top) {
        tss_set_kernel_stack(next->kernel_stack_top);
    }

    /* Switch address space if needed. Kernel mappings are mirrored in
     * every PD so kernel code/stacks stay reachable across the swap. */
    if (next->cr3 && next->cr3 != prev->cr3) {
        write_cr3(next->cr3);
    }

    task_switch(&prev->esp, next->esp);

    /* When this task is later resumed, control returns here. */
    __asm__ volatile ("sti");
}

void task_yield(void) {
    schedule();
}

struct task *task_current(void) {
    return g_current;
}

struct task *task_at(uint32_t slot) {
    if (slot >= TASK_MAX) return NULL;
    if (g_tasks[slot].state == TASK_STATE_UNUSED) return NULL;
    return &g_tasks[slot];
}

const char *task_state_name(int s) {
    switch (s) {
        case TASK_STATE_READY:   return "READY";
        case TASK_STATE_RUNNING: return "RUN";
        case TASK_STATE_BLOCKED: return "BLOCK";
        case TASK_STATE_DEAD:    return "DEAD";
        default:                 return "?";
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
    for (;;) {
        pit_sleep(200);

        for (int i = 1; i < TASK_MAX; i++) {
            struct task *t = &g_tasks[i];
            if (t == g_current)              continue;
            if (t->state != TASK_STATE_DEAD) continue;
            if (t->stack_base == NULL)       continue;  /* already reaped */

            /* Splice the dead task out of the round-robin ring. */
            __asm__ volatile ("cli");
            struct task *p = t->next;
            int safety = TASK_MAX * 2;
            while (p && p != t && p->next != t && safety--) p = p->next;
            if (p && p->next == t) p->next = t->next;
            __asm__ volatile ("sti");

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
    }
}

void task_reaper_start(void) {
    task_create(task_reaper, "reaper");
}
