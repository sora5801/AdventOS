/*
 * sleep — minimal sleep(1) clone for selftest harnesses.
 *
 *   sleep N    sleep N seconds (integer)
 *
 * Implementation note: pit_sleep in the AdventOS kernel hlt-loops
 * inside a single syscall — pending signals (SIGKILL from
 * shell.job.cancel) aren't checked until the syscall returns,
 * which means a `sleep 30` task can't be killed for 30 seconds.
 * To stay responsive to cancel, we sleep in 100 ms chunks instead
 * of one long syscall: between chunks the kernel sees the next
 * iret to ring 3 and delivers any pending signal. Cost: one extra
 * syscall every 100 ms (cheap compared to the wall clock).
 */
#include "libuser.h"

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int n = atoi(argv[1]);
    if (n < 0) n = 0;
    if (n > 600) n = 600;       /* 10 min hard cap — selftest hygiene */
    /* 100 ms × 10 × n iterations. The chunk size is small enough
     * that SIGKILL latency is bounded to ~100 ms but large enough
     * that we don't burn meaningful CPU just sleep-iterating. */
    uint32_t chunks = (uint32_t)n * 10;
    for (uint32_t i = 0; i < chunks; i++) {
        sys_sleep_ms(100);
    }
    return 0;
}
