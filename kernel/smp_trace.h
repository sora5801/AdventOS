/*
 * smp_trace.h — single-macro trace logging gated on -DSMP_TRACE.
 *
 * The default build (no flag) leaves every call site as a no-op,
 * so there is no runtime cost. Pass `SMP_TRACE=1` to build.sh
 * (which adds -DSMP_TRACE to the kernel's CFLAGS) to enable.
 *
 * Each emitted line looks like:
 *
 *     [smp] cpu0 bkl_lock acq pid=8
 *     [smp] cpu1 net_unlock rel
 *     [smp] cpu0 schedule pick pid=15 prev=8
 *
 * Conventions:
 *   - `[smp]` prefix makes the output grep-friendly.
 *   - `cpuN` is the LAPIC id of the emitting CPU (0 = BSP).
 *   - Use short verb-noun forms: `bkl_lock acq`, `bkl_unlock rel`,
 *     `net_lock acq/rel`, `sched_lock acq/rel`, `schedule pick`,
 *     `yield enter/exit`, `tcp_rx enter`, `sock_read enter/exit`.
 *
 * Why an inline-ish macro rather than a function:
 *   - Zero-arg, zero-tax in the default build (preprocessor strips).
 *   - The macro's args are evaluated only when SMP_TRACE is on, so
 *     formatting strings + arg sites don't bloat the disabled build.
 *
 * Safety notes:
 *   - Calls lapic_id(), which reads LAPIC MMIO. Before smp_init
 *     finishes (g_smp_ready == 0) the LAPIC isn't mapped on the AP
 *     side and lapic_id() can fault. The macro reads g_smp_ready
 *     first and falls back to cpu 0 — cheap branch, never faults.
 *   - kprintf takes the kprintf lock internally, so the trace
 *     itself serializes the output. Output may interleave with
 *     other kprintf streams; the `[smp]` prefix keeps it parseable.
 *
 * Adding new trace points: keep the message short (< ~60 chars
 * including the cpuN tag). High-frequency call sites (per-IRQ, per-
 * tick) generate a lot of serial output; the overhead masks tight
 * races. Use sparingly outside the locking/scheduling hot paths.
 */

#ifndef ADVENTOS_SMP_TRACE_H
#define ADVENTOS_SMP_TRACE_H

#ifdef SMP_TRACE

#include "kprintf.h"
#include "lapic.h"

extern volatile int g_smp_ready;   /* defined in task.c */

/* `SMP_LOG` is the macro callers invoke; the build flag is
 * `-DSMP_TRACE`. Two different names so the gcc `-D` doesn't
 * shadow the macro definition (the flag-as-define is just the
 * empty token "1" expanded inline, and our function-like macro
 * would chain-expand into a syntax error). */
#define SMP_LOG(fmt, ...)                                            \
    do {                                                             \
        unsigned int _smpt_cpu = g_smp_ready ? lapic_id() : 0u;      \
        kprintf("[smp] cpu%u " fmt "\n", _smpt_cpu, ##__VA_ARGS__);  \
    } while (0)

#else   /* SMP_TRACE not defined — default build, zero overhead */

#define SMP_LOG(fmt, ...)  ((void)0)

#endif

#endif /* ADVENTOS_SMP_TRACE_H */
