#ifndef ADVENTOS_TASK_H
#define ADVENTOS_TASK_H

#include "../include/types.h"

#define TASK_NAME_MAX  16
/* Bumped from 16 in session 50 — sshd forks deeply (sshd → per-conn
 * child → sh.elf -c → command exec), and that's on top of init's
 * resident httpd / httpsd / sshd daemons, the selftest sh, and the
 * inbound ssh.elf client during the loopback test. Six concurrent
 * processes in the pipeline plus kernel kthreads were enough to hit
 * the old 16-slot ceiling. 32 leaves comfortable headroom. */
#define TASK_MAX       32
#define TASK_STACK_SZ  0x4000     /* 16 KiB per kernel task */
/* Bumped from 8 in session 26 — a 4-stage pipeline opens 3 pipes (6 fds)
 * in the parent before forking; with stdin/stdout/stderr already at 0/1/2
 * that needed 9 slots. 16 lets longer pipelines through cleanly. */
#define TASK_MAX_FDS   16

/* Per-process file descriptor entry. fd 0/1/2 are wired up to console
 * stdin/stdout/stderr at task_create time; fd 3+ hold handles obtained
 * via SYS_OPEN, SYS_OPEN_W, SYS_SOCKET, SYS_PIPE, SYS_ACCEPT.
 *
 * The fd entry is kind-discriminated: `obj_idx` indexes the per-kind
 * table (fs / sock / pipe / tmpfs). Using one slot for all kinds keeps
 * dup2 trivial — the entry is just (kind, obj_idx, offset) and we
 * memcpy on dup2 (after bumping the underlying object's refcount). */
enum {
    FD_FREE   = 0,
    FD_STDIN,
    FD_STDOUT,
    FD_FS,
    FD_SOCK,
    FD_PIPE_R,
    FD_PIPE_W,
    FD_TMPFS,
    FD_PROCFS,        /* synthesized /proc files; obj_idx = (kind<<16)|pid */
    FD_PTY_M,         /* pseudo-terminal master (session 52); obj_idx = pty idx */
    FD_PTY_S,         /* pseudo-terminal slave  (session 52); obj_idx = pty idx */
};

/* Session 62 — per-fd flags.  Currently only bit 0 = O_NONBLOCK
 * (settable via SYS_FD_NB).  When set on a FD_SOCK, sys_accept and
 * sys_read return -1 immediately if there's nothing to do, instead
 * of looping task_yield until data arrives.  Required for the WM's
 * event loop: it can't afford to block on a single client connection
 * at 60 fps. */
#define FD_FL_NONBLOCK   0x01

struct task_fd {
    int      kind;
    int      obj_idx;      /* fs_idx / sock_idx / pipe_idx / tmpfs_idx */
    uint32_t offset;       /* used by FD_FS, FD_TMPFS                  */
    void    *fs_data;      /* FD_FS only: which fs_instance owns this  */
                           /* fd. NULL means boot fs.                  */
    uint32_t flags;        /* FD_FL_* bitmask (session 62)             */
};

enum {
    TASK_STATE_UNUSED            = 0,
    TASK_STATE_READY             = 1,
    TASK_STATE_RUNNING           = 2,
    TASK_STATE_BLOCKED           = 3,   /* on a sync-primitive wait queue */
    TASK_STATE_DEAD              = 4,   /* exited and has no waiting parent  */
    TASK_STATE_BLOCKED_ON_CHILD  = 5,   /* in sys_wait, no zombie child yet  */
    TASK_STATE_ZOMBIE            = 6,   /* exited, parent hasn't reaped it   */
    TASK_STATE_STOPPED           = 7,   /* SIGSTOP/SIGTSTP — paused until SIGCONT */
};

struct task {
    /* The first field MUST be esp — task_switch.S stores into it via a
     * pointer to the struct's start address. */
    uint32_t      esp;
    uint32_t      id;
    int           state;
    char          name[TASK_NAME_MAX];
    void         *stack_base;
    uint32_t      switches_in;       /* counter for `tasks` command */
    struct task  *next;              /* circular round-robin list */

    /* Userspace state. cr3==kernel cr3 means a kernel-only task. */
    uint32_t      cr3;
    uint32_t      kernel_stack_top;  /* loaded into TSS.esp0 on switch  */
    uint32_t      user_eip;          /* 0 for kernel-only tasks         */
    uint32_t      user_esp;          /* 0 for kernel-only tasks         */
    int           is_user;

    /* Used by mutex/wait-queue code. A task may be on at most one
     * wait queue at a time, so a single-link pointer is enough. */
    struct task  *wait_next;

    /* SMP scheduling. cpu is the LOGICAL CPU id this task is currently
     * running on (== cpu_local()->cpu_id of the dispatching CPU), or
     * -1 if not RUNNING anywhere. The scheduler skips tasks with
     * state == TASK_STATE_RUNNING when picking a next task — those are
     * already in flight on some CPU and shouldn't be double-dispatched.
     *
     * cpu_pin: -1 = any CPU (default for user tasks). Otherwise
     * pin-to-CPU id; only that CPU may pick this task. Used for the
     * BSP's bootstrap thread (slot 0) — its kernel stack is the boot
     * stack, which is BSP-only — and for each AP's idle task (which
     * runs on the AP's own kernel stack the trampoline allocated). */
    int           cpu;
    int           cpu_pin;
    int           is_idle;            /* 1 if this is a per-CPU idle task (off-ring fallback) */

    /* Open file descriptors. Slots 0/1/2 are pre-wired to stdin/
     * stdout/stderr; slot 3+ are available for SYS_OPEN. */
    struct task_fd fds[TASK_MAX_FDS];

    /* Process tree: parent_id == 0 means "no parent in user terms"
     * (kernel tasks, or pid 0 / kmain). exit_code is harvested by
     * sys_wait when the task is in TASK_STATE_ZOMBIE. */
    uint32_t      parent_id;
    int           exit_code;

    /* Signals (session 16). sig_pending is a bitmap of queued signals
     * (bit N set => SIG_N pending). sig_mask is the blocked-set —
     * pending signals in the masked set are deferred. sig_handlers[i]
     * is a user-VA function pointer (or SIG_DFL / SIG_IGN). sig_tramp
     * is the VA of the libuser sigreturn trampoline; the kernel
     * pushes it as the handler's return address. */
    uint32_t      sig_pending;
    uint32_t      sig_mask;
    void         *sig_handlers[32];   /* NSIG */
    void         *sig_tramp;

    /* Per-task user heap (session 17). heap_brk is the user-VA "one
     * past the last byte" of the mapped heap; sys_brk grows or shrinks
     * by mapping/unmapping pages between this and the user-set target.
     * Heap lives at [USER_HEAP_START, USER_HEAP_MAX); heap_brk starts
     * at USER_HEAP_START (zero pages mapped) and grows on demand
     * when the user-side malloc calls sys_brk. */
    uint32_t      heap_brk;

    /* Working directory (session 25). Index of the directory entry
     * in the global FS table that this task's relative paths resolve
     * against; FS_DIR_ROOT (= 0xFF, defined in fs.h) means the root.
     * Inherited verbatim by fork; preserved across exec — POSIX. */
    uint8_t       cwd_dir;

    /* mmap regions (session 24). Each entry covers a contiguous,
     * page-aligned range [va_start, va_end) backed by a file at
     * file_offset for file_length bytes (any tail past file_length
     * inside the last page reads as zero). Pages within the range
     * aren't backed by physical memory until first touch — the page
     * fault handler does the on-demand fs_read + paging_map_in.
     * mmap_brk bump-allocates VAs from USER_MMAP_START. */
    struct task_mmap {
        int      in_use;
        uint32_t va_start;
        uint32_t va_end;
        int      fs_idx;
        uint32_t file_offset;
        uint32_t file_length;
    } mmaps[8];
    uint32_t      mmap_brk;

    /* Job control (session 20). pgid groups related processes (a
     * pipeline = one pgrp); sid is the session a pgrp belongs to.
     * Both default to 0 (no pgrp / kernel task); a user task that
     * calls setsid() becomes its own session and pgrp leader (pgid
     * = sid = pid). Children inherit both via fork. */
    uint32_t      pgid;
    uint32_t      sid;

    /* Process credentials (session 47). uid 0 = root (the default
     * for kernel-spawned tasks); /etc/passwd assigns >0 uids to
     * regular users, and login.elf sets them via SYS_SETUID after
     * a successful password check. uid/gid inherit across fork
     * and survive exec — same as POSIX. SYS_SETUID is privileged
     * to root (uid 0); a non-root task that tries it gets -1. */
    uint16_t      uid;
    uint16_t      gid;

    /* ptrace state (session 57). tracer_pid is the pid of the task
     * that did PTRACE_TRACEME or PTRACE_ATTACH on us; 0 means not
     * traced. When `traced_stopped` is non-zero, we're sitting in
     * TASK_STATE_STOPPED waiting for the tracer to PTRACE_CONT /
     * STEP — and the saved kernel-stack iret frame at trap_frame
     * is what the tracer reads/writes via GETREGS / SETREGS.
     *
     * trap_frame points into THIS task's kernel stack — specifically
     * at the `struct registers` the INT3 / #DB handler pushed before
     * stopping us. The tracer reads it through our cr3-independent
     * kernel mapping (the kernel stack is identity-mapped). */
    uint32_t      tracer_pid;
    int           traced_stopped;     /* 1 = waiting for tracer */
    int           trap_signal;        /* SIGTRAP / SIGSEGV etc.   */
    void         *trap_frame;         /* struct registers *       */

    /* Session 70: syscall sandbox.
     *
     * `sandbox_mask[i]` is the allow-bitmap covering syscalls
     * (i*32 .. i*32+31). Bit set = permitted, bit clear = denied.
     * `sandbox_active` is 0 when no policy has been installed
     * (all syscalls permitted, the default); 1 once SYS_SANDBOX_INSTALL
     * has been called even once. The mask is sticky and monotonic
     * (each install AND-s into the current value) and inherited
     * verbatim across fork+exec.
     *
     * `sandbox_denials` is a simple counter bumped on each blocked
     * syscall — exposed via procfs and useful for catching unexpected
     * policy mismatches in development. */
    uint32_t      sandbox_mask[4];    /* SANDBOX_MASK_WORDS = 4 */
    int           sandbox_active;
    uint32_t      sandbox_denials;
};

#define USER_HEAP_START  0x40200000u
#define USER_HEAP_MAX    0x40600000u   /* 4 MiB cap per process */

#define USER_MMAP_START  0x50000000u
#define USER_MMAP_MAX    0x60000000u   /* 256 MiB window for mappings */
#define TASK_MMAP_MAX    8

typedef void (*task_fn)(void);

void           task_init(void);
struct task   *task_create(task_fn entry, const char *name);

/* task_create returns the task in TASK_STATE_BLOCKED so the caller
 * can finish field setup (e.g., user task's cr3/eip/esp) without
 * the scheduler dispatching a half-built task on another CPU.
 * Call task_make_runnable() once the task is fully built — this
 * flips state to READY and the scheduler will pick it up. */
void           task_make_runnable(struct task *t);

/* Build the per-CPU idle task for an application processor. Called
 * from ap_entry once the AP has its kernel stack + LAPIC + TSS. The
 * resulting TCB is marked is_idle (off the round-robin ring), bound
 * to the supplied CPU id, and immediately returned as RUNNING — so
 * the AP is "running idle" by the time it enables interrupts. */
struct task   *task_init_ap_idle(uint32_t cpu_id, uint32_t kernel_stack_top,
                                 void *kernel_stack_base);

/* Called once from kmain after smp_init has mapped the LAPIC and
 * populated the per-CPU table. From this point on, scheduler hot
 * paths can safely call cpu_local() (which reads LAPIC MMIO). */
void           task_smp_ready(void);

/* Spawn a ring-3 task. The caller has already mapped user_eip and the
 * stack page in the supplied page directory. */
struct task   *task_create_user(uint32_t user_eip, uint32_t user_esp,
                                uint32_t cr3, const char *name);

void           task_yield(void);
void           schedule(void);

struct task   *task_current(void);
struct task   *task_at(uint32_t slot);   /* nullable */
const char    *task_state_name(int state);

/* Spawn the kernel reaper. Call once, after task_init + sti. The
 * reaper periodically scans for DEAD tasks and frees their kernel
 * stack and (if user) user PD + user pages, returning the slot to
 * UNUSED so it can be reused by a future task_create. */
void           task_reaper_start(void);

/* Tell the task layer which pid to reparent orphans to (session 22).
 * kmain calls this right after spawning init.elf. While the value is
 * 0 — i.e. before init exists — orphans go straight to DEAD as
 * before, so the kernel reaper can clean them up. */
void           task_set_init_pid(uint32_t pid);
uint32_t       task_get_init_pid(void);

/* ---- fork / exec / wait support ----------------------------------- */

struct registers;   /* from isr.h */

/* Clone the calling task: deep-copy its user PD, snapshot its fd
 * table, and synthesize a child kernel stack such that the child's
 * first scheduling resumes at the parent's saved user EIP/ESP with
 * EAX = 0. Returns the child task pointer (whose ->id is the child
 * pid) or NULL on failure. The supplied register frame is the parent's
 * INT 0x80 entry frame — the child will iret to those exact values. */
struct task   *task_fork(struct registers *parent_regs);

/* Replace the calling task's address space with a freshly-loaded ELF.
 * The old user PD is freed; a new one is built; argv is packed onto
 * the new stack; the parent's syscall-return register frame is
 * rewritten so the iret at the end of the syscall lands in the new
 * program's _start with the new ESP. Returns 0 on success, negative
 * error code on failure (in which case the caller's address space is
 * untouched).
 *
 * `argv_strs` is a kernel-side array of NUL-terminated strings —
 * the caller must have already snapshotted them out of user memory. */
int            task_exec_inplace(struct registers *r,
                                 const char *path,
                                 int argc,
                                 const char *const *argv_strs);

/* Mark the calling task as having exited. Records exit_code, wakes a
 * BLOCKED_ON_CHILD parent (if any) and demotes the task to ZOMBIE
 * (parent will reap via wait) or DEAD (kernel will reap). Never
 * returns; caller must follow with schedule() / hlt-loop. */
void           task_exit_current(int exit_code);

/* Pull a single zombie child of the calling task off the table:
 * harvest its exit code into *out_code and demote it to DEAD so the
 * kernel reaper frees it. If no zombie child exists, blocks the
 * calling task in BLOCKED_ON_CHILD and yields until one does (or
 * returns -1 immediately if no children at all). */
int            task_wait_current(int *out_code);

/* Non-blocking variant of task_wait_current. Returns:
 *    pid > 0   a zombie child was harvested
 *    0         a child exists but no zombie is ready right now
 *    -1        no children at all
 *
 * Used by fork-per-conn servers (e.g. httpd) to drain zombies in
 * between accepts without committing to a blocking wait. Mirrors
 * Linux waitpid(-1, ..., WNOHANG). */
int            task_waitpid_nb_current(int *out_code);

#endif
