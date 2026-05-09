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
};

struct task_fd {
    int      kind;
    int      fs_idx;       /* used iff kind == FD_FS */
    uint32_t offset;       /* used iff kind == FD_FS */
};

enum {
    TASK_STATE_UNUSED  = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3,   /* on a sync-primitive wait queue */
    TASK_STATE_DEAD    = 4,
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

#endif
