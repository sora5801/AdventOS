#include "netlock.h"
#include "spinlock.h"
#include "smp.h"
#include "lapic.h"

/* Inner non-recursive spinlock. spin_lock CLIs on this CPU and
 * busy-spins waiting for the lock byte to become 0; spin_unlock
 * restores IF only if the caller had it set at acquire time. */
static spinlock_t g_inner = SPINLOCK_INIT;

/* Recursion bookkeeping. Both are touched only under the inner
 * spinlock (or by an owner that's about to take it) — owner_cpu is
 * latched after a successful spin_lock so a peer CPU that just
 * acquired sees its own id. depth tracks how many net_lock calls
 * deep we are on the owner CPU. */
static volatile int g_owner_cpu = -1;
static volatile int g_depth     = 0;

static int my_cpu(void) {
    extern volatile int g_smp_ready;
    /* Pre-SMP code path can call into TCP (e.g. dhcp/dns during init).
     * cpu_local()/lapic_id() rely on MMIO that's only mapped after
     * smp_init, so before the flip we always say "CPU 0". */
    return g_smp_ready ? (int)lapic_id() : 0;
}

void net_lock(void) {
    int me = my_cpu();
    /* Read-only peek at owner_cpu first — if it's already us, we just
     * bump depth without re-acquiring the inner spinlock. Safe to read
     * unlocked because:
     *   (a) Another CPU can't set owner_cpu to OUR id (only we can).
     *   (b) If owner_cpu transitions away from us (we wouldn't have
     *       set it to begin with) we wouldn't be in this branch.
     * The volatile read keeps the compiler from caching it. */
    if (g_owner_cpu == me) {
        g_depth++;
        return;
    }
    spin_lock(&g_inner);
    g_owner_cpu = me;
    g_depth     = 1;
}

void net_unlock(void) {
    if (g_depth == 0) {
        /* Defensive: somebody called net_unlock without a matching
         * net_lock. Leave the lock alone — fixing the misuse is the
         * caller's job. A future kprintf-on-misuse would be nice. */
        return;
    }
    if (--g_depth > 0) return;          /* still nested */
    g_owner_cpu = -1;
    spin_unlock(&g_inner);
}

int net_lock_owned_by_me(void) {
    return g_owner_cpu == my_cpu();
}
