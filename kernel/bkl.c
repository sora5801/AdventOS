#include "bkl.h"
#include "spinlock.h"
#include "smp.h"
#include "lapic.h"
#include "smp_trace.h"

/* The lock itself. saved_eflags inside spinlock_t records the
 * acquirer's IF state so unlock restores it correctly. */
static spinlock_t g_bkl = SPINLOCK_INIT;

/* Per-CPU "I hold the BKL" flag. Used by bkl_held() for assertions
 * and by syscall_dispatch's drop-on-yield helper to know whether
 * to even attempt unlock. */
static volatile int g_bkl_owner_cpu;
static volatile int g_bkl_held;

void bkl_lock(void) {
    SMP_LOG("bkl_lock try");
    spin_lock(&g_bkl);
    /* spin_lock cli's, so writes here are uninterrupted on this CPU.
     * Other CPUs can't observe these writes anyway because they're
     * spinning on the lock. */
    g_bkl_held = 1;
    extern volatile int g_smp_ready;
    g_bkl_owner_cpu = g_smp_ready ? (int)lapic_id() : 0;
    SMP_LOG("bkl_lock acq");
}

void bkl_unlock(void) {
    SMP_LOG("bkl_unlock rel");
    g_bkl_held = 0;
    g_bkl_owner_cpu = -1;
    spin_unlock(&g_bkl);
}

int bkl_held(void) {
    if (!g_bkl_held) return 0;
    extern volatile int g_smp_ready;
    int my = g_smp_ready ? (int)lapic_id() : 0;
    return g_bkl_owner_cpu == my;
}
