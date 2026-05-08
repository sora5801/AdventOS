#ifndef ADVENTOS_SPINLOCK_H
#define ADVENTOS_SPINLOCK_H

#include "../include/types.h"

/*
 * Single-CPU spinlock that doubles as an IRQ-disable section. We
 * record the caller's EFLAGS at acquire time so unlock restores the
 * original IF — this lets nested locks compose correctly even when
 * one of them happens inside an IRQ handler (where IF was already 0).
 *
 * On a uniprocessor this never actually spins: the cli prevents the
 * holder from being preempted, so any apparent contention is a bug
 * (re-entrance, IRQ trying to take a lock the main flow holds, etc).
 */

typedef struct {
    volatile uint32_t locked;
    uint32_t          saved_eflags;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock_init(spinlock_t *l) {
    l->locked       = 0;
    l->saved_eflags = 0;
}

static inline uint32_t spin_xchg(volatile uint32_t *p, uint32_t v) {
    uint32_t out;
    __asm__ volatile ("xchgl %0, %1"
                      : "+m"(*p), "=a"(out)
                      : "1"(v)
                      : "cc", "memory");
    return out;
}

static inline void spin_lock(spinlock_t *l) {
    uint32_t flags;
    __asm__ volatile ("pushfl\n\t"
                      "popl %0\n\t"
                      "cli"
                      : "=r"(flags) :: "memory");
    while (spin_xchg(&l->locked, 1) != 0) {
        __asm__ volatile ("pause");
    }
    l->saved_eflags = flags;
}

static inline void spin_unlock(spinlock_t *l) {
    uint32_t flags = l->saved_eflags;
    spin_xchg(&l->locked, 0);
    if (flags & 0x200) __asm__ volatile ("sti");
}

#endif
