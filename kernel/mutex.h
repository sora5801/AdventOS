#ifndef ADVENTOS_MUTEX_H
#define ADVENTOS_MUTEX_H

#include "../include/types.h"

struct task;

/*
 * Sleeping mutex. Tasks that fail to acquire are placed on the wait
 * queue, marked TASK_STATE_BLOCKED, and yield via schedule(). On
 * unlock the head waiter has the lock transferred to it directly
 * (the lock never goes back to the "free" state in between) and is
 * marked READY for the next round-robin tick to pick up.
 */

typedef struct mutex {
    int          locked;
    struct task *holder;
    struct task *waiters_head;
    struct task *waiters_tail;
} mutex_t;

#define MUTEX_INIT { 0, NULL, NULL, NULL }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int  mutex_try_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

#endif
