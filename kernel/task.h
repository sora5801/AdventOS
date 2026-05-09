#ifndef ADVENTOS_TASK_H
#define ADVENTOS_TASK_H

#include "../include/types.h"

#define TASK_NAME_MAX  16
#define TASK_MAX       16
#define TASK_STACK_SZ  0x4000     /* 16 KiB per kernel task */
#define TASK_MAX_FDS   8

/* Per-process file descriptor entry. fd 0/1/2 are wired up to console
 * stdin/stdout/stderr at task_create time; fd 3+ hold filesystem-backed
 * handles obtained via SYS_OPEN. */
enum {
    FD_FREE   = 0,
    FD_STDIN,
    FD_STDOUT,
    FD_FS,
    FD_SOCK,
};

struct task_fd {
    int      kind;
    int      fs_idx;       /* used iff kind == FD_FS    */
    uint32_t offset;       /* used iff kind == FD_FS    */
    int      sock_idx;     /* used iff kind == FD_SOCK  */
};

enum {
    TASK_STATE_UNUSED            = 0,
    TASK_STATE_READY             = 1,
    TASK_STATE_RUNNING           = 2,
    TASK_STATE_BLOCKED           = 3,   /* on a sync-primitive wait queue */
    TASK_STATE_DEAD              = 4,   /* exited and has no waiting parent  */
    TASK_STATE_BLOCKED_ON_CHILD  = 5,   /* in sys_wait, no zombie child yet  */
    TASK_STATE_ZOMBIE            = 6,   /* exited, parent hasn't reaped it   */
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

    /* Open file descriptors. Slots 0/1/2 are pre-wired to stdin/
     * stdout/stderr; slot 3+ are available for SYS_OPEN. */
    struct task_fd fds[TASK_MAX_FDS];

    /* Process tree: parent_id == 0 means "no parent in user terms"
     * (kernel tasks, or pid 0 / kmain). exit_code is harvested by
     * sys_wait when the task is in TASK_STATE_ZOMBIE. */
    uint32_t      parent_id;
    int           exit_code;
};

typedef void (*task_fn)(void);

void           task_init(void);
struct task   *task_create(task_fn entry, const char *name);

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

#endif
