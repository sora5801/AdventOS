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
#include "../include/io.h"

extern void task_switch(uint32_t *old_esp, uint32_t new_esp);
extern void fork_child_return(void);     /* asm trampoline in task_switch.S */

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

    /* Pre-wire fd 0/1/2 to stdin/stdout/stderr. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        t->fds[i].kind     = FD_FREE;
        t->fds[i].sock_idx = -1;
    }
    t->fds[0].kind = FD_STDIN;
    t->fds[1].kind = FD_STDOUT;
    t->fds[2].kind = FD_STDOUT;

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

    /* Pick the next runnable task. Skip anything that isn't currently
     * runnable: DEAD, ZOMBIE (exited), BLOCKED (mutex), BLOCKED_ON_CHILD
     * (in sys_wait). */
    struct task *next = g_current->next;
    int safety = TASK_MAX;
    while (next != g_current
           && (next->state == TASK_STATE_DEAD              ||
               next->state == TASK_STATE_ZOMBIE            ||
               next->state == TASK_STATE_BLOCKED           ||
               next->state == TASK_STATE_BLOCKED_ON_CHILD)
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
        case TASK_STATE_READY:            return "READY";
        case TASK_STATE_RUNNING:          return "RUN";
        case TASK_STATE_BLOCKED:          return "BLOCK";
        case TASK_STATE_DEAD:             return "DEAD";
        case TASK_STATE_BLOCKED_ON_CHILD: return "WAIT";
        case TASK_STATE_ZOMBIE:           return "ZOMB";
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
    struct task *parent = g_current;
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
     *    robin ring so the scheduler never sees a half-built task. */
    struct task *child = &g_tasks[slot];
    memset(child, 0, sizeof(*child));
    child->id               = g_next_id++;
    child->state            = TASK_STATE_READY;
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

    /* 5. Inherit fd table verbatim. fd offsets are per-fd (no shared
     *    open-file table), so child gets an independent file position;
     *    socket fds work but a double-close becomes the second close
     *    failing rather than UB — acceptable for our shell which never
     *    forks while a socket is open. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        child->fds[i] = parent->fds[i];
    }

    /* 6. Splice into the round-robin ring. */
    __asm__ volatile ("cli");
    child->next       = parent->next;
    parent->next      = child;
    __asm__ volatile ("sti");

    return child;
}

int task_exec_inplace(struct registers *r,
                      const char *path,
                      int argc,
                      const char *const *argv_strs) {
    struct task *t = g_current;
    if (!t->is_user) return -1;

    /* Open + load the new ELF FIRST, into a fresh PD. If anything
     * fails we abort with the caller's address space untouched. */
    int fd = fs_open(path);
    if (fd < 0) return -2;

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

void task_exit_current(int exit_code) {
    struct task *t = g_current;
    t->exit_code = exit_code;

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
        __asm__ volatile ("cli");
        struct task *p = c->next;
        int safety = TASK_MAX * 2;
        while (p && p != c && p->next != c && safety--) p = p->next;
        if (p && p->next == c) p->next = c->next;
        __asm__ volatile ("sti");

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
    struct task *t = g_current;
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
