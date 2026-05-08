#include "mutex.h"
#include "task.h"

void mutex_init(mutex_t *m) {
    m->locked       = 0;
    m->holder       = NULL;
    m->waiters_head = NULL;
    m->waiters_tail = NULL;
}

int mutex_try_lock(mutex_t *m) {
    int got = 0;
    __asm__ volatile ("cli");
    if (!m->locked) {
        m->locked = 1;
        m->holder = task_current();
        got = 1;
    }
    __asm__ volatile ("sti");
    return got;
}

void mutex_lock(mutex_t *m) {
    __asm__ volatile ("cli");

    if (!m->locked) {
        m->locked = 1;
        m->holder = task_current();
        __asm__ volatile ("sti");
        return;
    }

    /* Couldn't get it — enqueue ourselves and block. */
    struct task *self = task_current();
    self->wait_next = NULL;
    if (m->waiters_tail) {
        m->waiters_tail->wait_next = self;
    } else {
        m->waiters_head = self;
    }
    m->waiters_tail = self;
    self->state = TASK_STATE_BLOCKED;

    /* schedule() picks another runnable task. We come back here only
     * after mutex_unlock has dequeued us, transferred ownership, and
     * the round-robin scheduler picks us up again. At that point we
     * already hold the lock — there's no need to re-check. */
    schedule();
    /* schedule() leaves IF=1, so we're already back in normal state. */
}

void mutex_unlock(mutex_t *m) {
    __asm__ volatile ("cli");

    if (m->waiters_head) {
        struct task *w   = m->waiters_head;
        m->waiters_head  = w->wait_next;
        if (!m->waiters_head) m->waiters_tail = NULL;
        w->wait_next     = NULL;
        w->state         = TASK_STATE_READY;
        m->holder        = w;
        /* m->locked stays 1 — ownership transferred directly. */
    } else {
        m->locked = 0;
        m->holder = NULL;
    }

    __asm__ volatile ("sti");
}
