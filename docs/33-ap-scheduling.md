# Session 33 — AP scheduling: putting the second CPU to work

**Goal:** Stop wasting the AP. Session 31 brought the application processor online — got it through INIT-SIPI-SIPI, into 32-bit paged kernel C with its own TSS and stack — but parked it in a `cli; hlt` loop because the scheduler was BSP-only. This session wires the AP into the scheduler. Each CPU has its own `current` task pointer, a per-CPU LAPIC-timer-driven preemption tick, and a private idle thread. A global scheduler lock serializes the run-queue across CPUs, with a "lock-handoff across `task_switch`" trick that keeps `prev->esp` safe from racing dispatchers.

End state — the new boot lines:

```
[smp] starting AP1 (apic_id=1)
[smp] AP1 online (apic_id=1, lapic-timer @ vec 0x40)
[smp] 2/2 CPU(s) online
```

And the rewritten `[t22]` selftest:

```
[t22] SMP: APs run kernel tasks via LAPIC-timer preemption
  cpu count: 2, 300ms deltas:
    cpu0: 0 lapic-timer ticks, 36 task dispatches
    cpu1: 30 lapic-timer ticks, 30 task dispatches
  PASS: AP1 dispatched 30 kernel tasks in 300ms
  shell pid=7 on CPU apic_id=0 (user tasks pinned to BSP)
  child 0 (pid=8) on CPU apic_id=0
  child 1 (pid=9) on CPU apic_id=0
```

The numbers tell the story. AP1 is firing its LAPIC timer at ~100Hz and each tick goes into the scheduler, which picks a real kernel task (`demo_task_a`/`demo_task_b`/`reaper`) off the round-robin ring 30 times in 300ms. The BSP is doing the same via the legacy PIT (no LAPIC timer ticks visible — its preemption is still PIT-driven, but you can see the dispatch counter advancing). All 23 selftests still pass under `-smp 2`; `curl http://localhost:8200/` continues to serve `httpd.elf`.

What's NOT in this session: AP picking up user tasks. The kernel's syscall surface (fs.c, bcache, paging_destroy_user_pd, the elf loader) holds a lot of unsynchronized global state, and letting two CPUs into it concurrently produced reliable NULL-deref'd crashes during `init.elf`'s `exec("httpd.elf")`. We pin user tasks to the BSP via a per-task `cpu_pin` field — the same machinery the per-CPU idles use — and defer SMP-safe syscalls to a future session. The scheduling infrastructure is in place; user-task migration is one Big Kernel Lock (or a few targeted spinlocks) away.

This deep dive walks through: the per-CPU current pointer, the LAPIC timer setup and ISR, the global scheduler lock with lock-handoff across `task_switch`, the trampoline that releases the lock for new tasks, and the four bugs that took the day to pin down.

## What's in scope

In:
- **`kernel/task.{h,c}`** — Per-CPU `current` (via `cpu_local()->current`), global `g_sched_lock` (`spinlock.h`), `t->cpu` and `t->cpu_pin` fields, `pick_next_ready` that respects pinning, `task_init_ap_idle()` to build per-AP idle TCBs, `task_smp_ready()` to flip the LAPIC-MMIO-is-mapped flag, `post_switch_finalize()` for the new-task lock-release trampoline.
- **`kernel/task_switch.S`** — `_task_entry_trampoline` that calls `_post_switch_finalize` and `ret`s into the real entry function. Same hook prepended to `_fork_child_return`.
- **`kernel/lapic.{h,c}`** — `lapic_timer_init(initial_count)` programs the per-CPU timer in periodic mode at vector 0x40; `lapic_timer_stop()` masks it.
- **`kernel/isr_stubs.S`** — `_lapic_timer_stub` at IDT vector 0x40 that routes to `_lapic_irq_handler` via a parallel-but-LAPIC-EOI'd `lapic_irq_common_stub`.
- **`kernel/isr.c`** — `lapic_irq_handler()`: `lapic_eoi()`, increment per-CPU tick counter, `schedule()`, signal-delivery hook. Same shape as the PIT path.
- **`kernel/idt.c`** — Wires IDT vector 0x40 to `_lapic_timer_stub`.
- **`kernel/smp.{h,c}`** — `cpu_local` gains an `idle` field; `ap_entry()` builds a per-CPU idle TCB, programs the LAPIC timer, and enters its `sti; hlt` idle loop.
- **`kernel/tss.{h,c}`** — `tss_set_kernel_stack()` dispatches to BSP's `g_tss` or AP's `cpu_local()->tss` based on the calling CPU.
- **`kernel/syscall.{h,c}`** — `SYS_SMP_STATS = 53` returns per-CPU LAPIC-timer tick counts and dispatch counts; used by the selftest.
- **`user/sh.c`** — `[t22]` rewritten to verify per-CPU dispatch counts and document the user-task pin.

Out:
- **AP picking up user tasks.** Pinned to BSP via `cpu_pin = 0`. The page-fault races we hit when AP grabbed a forked child were rooted in fs/elf/paging-destroy global state; lifting the pin requires SMP-safe syscalls.
- **TLB shootdowns.** With user tasks pinned to BSP, no PT a CPU writes is ever cached on a different CPU. When user-task migration ships, this becomes a real concern.
- **BSP using its own LAPIC timer.** The BSP still uses the legacy PIT for preemption. Switching it over is one `lapic_timer_init()` call away but pointless until user-task scheduling is on AP — neither timer is the bottleneck.
- **CPU affinity beyond pin-to-CPU-N.** No load-balancer, no NUMA awareness, no work-stealing. The round-robin is fair but blind.
- **Calibrated timer frequency.** We hard-code `initial_count = 10_000_000` LAPIC-bus-clocks (≈10ms on QEMU). Real hardware would want a calibration loop against the PIT.

## Architecture: one scheduler, two CPUs

```
                     BSP (CPU 0)                          AP (CPU 1)
                     ─────────────                        ─────────────
  IDLE / kmain (cpu_pin=0, in ring) ─┐         AP idle (cpu_pin=1, off-ring)
                                     │                            │
            PIT IRQ @ 100Hz          │       LAPIC timer @ ~100Hz │
                  │                  │              │             │
                  ▼                  │              ▼             │
            irq0 stub                │       lapic_timer_stub     │
                  │                  │              │             │
            irq_handler (EOI 8259)   │       lapic_irq_handler    │
                  │                  │           (lapic_eoi)      │
                  ▼                  │              ▼             │
              schedule()             │          schedule()        │
                  │                  │              │             │
                  └──────┐           │              ┌─────────────┘
                         ▼           ▼              ▼
                              spin_lock(g_sched_lock)
                         ┌─────────────────────┴─────────────────────┐
                         │ 1. demote prev (if RUNNING && !is_idle)   │
                         │ 2. pick next: ring traversal,             │
                         │      skip RUNNING, skip cpu_pin mismatch  │
                         │ 3. fall back to cpu->idle                 │
                         │ 4. set next->state = RUNNING              │
                         │      next->cpu  = my_cpu                  │
                         │      cpu->current = next                  │
                         │ 5. tss_set_kernel_stack(next->stack_top)  │
                         │ 6. write_cr3(next->cr3)  if changed       │
                         │ 7. task_switch(&prev->esp, next->esp)     │
                         │      ── HOLDING THE LOCK ACROSS THIS ──   │
                         │                                            │
                         │  ── on resume of an old task: ─────       │
                         │ 8. spin_unlock(g_sched_lock)              │
                         │  ── on first run of a new task: ─────     │
                         │ 8'. trampoline: post_switch_finalize();   │
                         │     ret → real entry                       │
                         └────────────────────────────────────────────┘
```

Three mechanisms work together:

1. **Per-CPU `current`** (`cpu_local()->current`) replaces the old single global `g_current`. A CPU asks "who's running on me?" by reading its own LAPIC ID and indexing the per-CPU table.
2. **Global scheduler lock** (`g_sched_lock`) makes the run-queue, `t->state` transitions, and `cpu->current` writes serialize across CPUs.
3. **LAPIC timer per-CPU** drives preemption on the AP — the BSP has had the PIT all along; the AP needs its own clock because the legacy 8259 PIC routes IRQ0 to the BSP only.

## The per-CPU current

`g_current` is gone. In its place, `cpu_local()->current` — pulled from the per-CPU table indexed by LAPIC ID. `task_current()` becomes:

```c
struct task *task_current(void) {
    return cpu_current();
}

static inline struct task *cpu_current(void) {
    if (!g_smp_ready) return &g_tasks[0];
    struct cpu_local *c = cpu_local();
    return c->current ? c->current : &g_tasks[0];
}
```

The `g_smp_ready` flag matters more than it looks. `cpu_local()` reads LAPIC MMIO (`*(volatile uint32_t *)(LAPIC_VIRT_BASE + LAPIC_REG_ID) >> 24`), and the LAPIC page isn't mapped until `lapic_init()` runs as part of `smp_init()`. Anything that calls `cpu_local()` before that point page-faults on a kernel-mode access to `0xFEE00020`. The boot sequence has `task_init()` running BEFORE `smp_init()`, and the very first thing the original `task_init()` did in my SMP rewrite was `cpu_local()->current = &g_tasks[0]` — which crashed instantly.

The fix: `g_smp_ready` defaults to 0; `task_smp_ready()` (called from `kmain` right after `smp_init()` returns) flips it to 1. Pre-flip, all the per-CPU dispatch sites short-circuit to "you're CPU 0 / it's the BSP idle / use `g_tss` directly". Post-flip, they go through `cpu_local()` which now safely reads the mapped LAPIC.

The same flag gates `tss_set_kernel_stack()`:

```c
extern volatile int g_smp_ready;
void tss_set_kernel_stack(uint32_t esp) {
    if (!g_smp_ready) {
        g_tss.ss0  = 0x10;
        g_tss.esp0 = esp;
        return;
    }
    struct cpu_local *cpu = cpu_local();
    if (cpu->cpu_id == 0) {
        g_tss.ss0  = 0x10;
        g_tss.esp0 = esp;
    } else {
        cpu->tss.ss0  = 0x10;
        cpu->tss.esp0 = esp;
    }
}
```

The BSP still uses `g_tss` (the GDT entry at selector 0x28 `gdt_init` set up). Each AP uses its own `cpu_local()->tss` (the GDT entry `smp_init` allocated via `gdt_add_tss(&c->tss)`). This split is purely historical — the BSP's GDT was wired to `g_tss` long before per-CPU TSSes existed and there's no reason to refactor it; we just dispatch.

## The LAPIC timer

`lapic_timer_init(uint32_t initial_count)` programs the calling CPU's local timer:

```c
void lapic_timer_init(uint32_t initial_count) {
    /* Set divider FIRST. Some CPUs latch the LVT entry's vector at
     * the moment LVT is written; if we wrote LVT before the divider
     * the first tick could fire with a stale period. */
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIVIDER);
    lapic_write(LAPIC_REG_LVT_TIMER,
                LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT, initial_count);
}
```

Three registers:

- **`LAPIC_REG_TIMER_DIV` (0x3E0)** — divides the LAPIC bus clock. We use `0xB` = divide by 1.
- **`LAPIC_REG_LVT_TIMER` (0x320)** — Local Vector Table entry for the timer. Bits: vector (8 bits), delivery mode (3 bits), mask (bit 16), timer mode (bits 17-18: 00 one-shot, 01 periodic, 10 TSC-deadline). We set vector = `0x40` and periodic mode.
- **`LAPIC_REG_TIMER_INIT` (0x380)** — initial count. Counts down at the divided bus clock. When it hits 0, IRQ fires; in periodic mode it reloads from this register and counts down again.

We pick `initial_count = 10_000_000`. On QEMU that's roughly 10ms (QEMU doesn't precisely model the LAPIC bus clock). The selftest measured 30 dispatches over a 300ms sleep — exactly 100Hz, confirming the rate.

The IDT is wired in `idt.c`:

```c
idt_set_gate(0x40, (uint32_t)lapic_timer_stub, 0x08, 0x8E);
```

DPL=0 — ring 3 can't trigger this manually.

The asm stub is just like an IRQ stub but routes to a separate handler:

```asm
.global _lapic_timer_stub
_lapic_timer_stub:
    cli
    push    $0
    push    $0x40
    jmp     lapic_irq_common_stub
```

`lapic_irq_common_stub` is identical to `irq_common_stub` except it calls `_lapic_irq_handler` instead of `_irq_handler`. The C handler:

```c
void lapic_irq_handler(struct registers *r) {
    lapic_eoi();
    if (g_smp_ready) {
        struct cpu_local *cpu = cpu_local();
        if (cpu->cpu_id < 8) g_lapic_tick_count[cpu->cpu_id]++;
    }
    schedule();
    signal_check_and_deliver(r);
}
```

The EOI goes to the LAPIC, not to the legacy 8259 PIC. The LAPIC's EOI register is at offset 0xB0 — any write completes the interrupt. The PIC EOI we send for IRQ0..15 is irrelevant to LAPIC vectors.

The handler structure mirrors the PIT path (EOI first because `schedule()` may not return). Per-CPU tick counts feed the `[t22]` diagnostic.

## The lock-handoff across `task_switch`

The hardest part of SMP scheduling on this architecture is the prev-task-state-save race. Concretely:

- CPU A picks `next`, marks `prev` as READY so other CPUs can see it's available, switches CR3 + TSS, calls `task_switch(&prev->esp, next->esp)`.
- `task_switch` saves CPU A's current ESP into `prev->esp`, then loads `next->esp`. Between "set prev to READY" and "save prev->esp", `prev` is in an inconsistent state — its `state == READY` advertises "I'm pickable" but `prev->esp` still points wherever it pointed last time prev was suspended. If CPU B sees `prev` as READY in this window and dispatches it via `task_switch(&...->esp, prev->esp)`, CPU B reads stale stack.

There are several workable solutions:

1. **Don't transition `prev->state` to READY until after `prev->esp` is saved.** Requires a hook inside `task_switch.S` between the save and the load — workable but invasive.
2. **Per-CPU run-queues.** Each CPU has its own queue; cross-CPU work goes through an IPI. Linux-style. Way too much surgery for one session.
3. **Hold the global scheduler lock across `task_switch`.** No other CPU can touch the run queue while we're mid-switch. The catch: who releases the lock on the other side?

We do (3). The trick: the lock is "logically held" across the switch — it's a global flag, no CPU owns it — and the incoming code path on the destination side is responsible for dropping it. There are three destination paths:

| Path | Where it lands | Who drops the lock |
|------|----------------|-------------------|
| Resumed previously-suspended task | `spin_unlock` after `task_switch` in `schedule()` | The `spin_unlock` call directly |
| Brand-new kernel task | `_task_entry_trampoline` (asm) | Trampoline calls `post_switch_finalize` |
| Forked child | `_fork_child_return` (asm) | Same: prepended `call _post_switch_finalize` |

`schedule()`'s tail looks like:

```c
    /* Hand-off the lock across task_switch. The lock stays HELD; when
     * the incoming task (`next`) eventually returns from its own past
     * task_switch call site below, IT will spin_unlock. */
    task_switch(&prev->esp, next->esp);

    /* Resumed here LATER, on whichever CPU now runs prev. Drop the
     * lock to let the rest of the kernel run. */
    spin_unlock(&g_sched_lock);
```

For the brand-new-task path, `task_create()` synthesizes a stack with the trampoline as `task_switch`'s `ret` target:

```
    high addr ─┬─────────────────────────────────────┐
               │ 0          (sentinel; if entry      │
               │             returns, ret to 0       │
               │             page-faults loudly)     │
               │ entry      (trampoline's ret jumps  │
               │             here — real function)   │
               │ trampoline (task_switch's ret jumps │
               │             here first)             │
               │ EFLAGS = 0x202 (IF=1 + reserved)    │
               │ EBP = 0                              │
               │ EBX = 0                              │
               │ ESI = 0                              │
               │ EDI = 0                              │
    low addr   └─────────────────────────────────────┘
                 ↑ task->esp
```

The trampoline (in `task_switch.S`):

```asm
.global _task_entry_trampoline
_task_entry_trampoline:
    call    _post_switch_finalize    /* release g_sched_lock */
    ret                              /* pops real entry, jumps */
```

`post_switch_finalize` is just `spin_unlock(&g_sched_lock)`. After it returns, the `ret` pops the real entry function pointer and tail-calls it.

For the fork path, the trampoline-style hook is prepended to the existing `_fork_child_return`:

```asm
.global _fork_child_return
_fork_child_return:
    call    _post_switch_finalize  /* release g_sched_lock */
    pop     %eax                /* saved DS */
    mov     %ax, %ds
    ...
    iret                        /* back to ring 3 at parent's EIP */
```

There's still an asymmetry to worry about: the spinlock saves the holder's EFLAGS at acquire time so unlock can restore IF. With lock-handoff, the *acquiring* CPU and the *releasing* CPU are different. The saved EFLAGS in the lock struct reflects the *acquirer's* IF. When the releasing CPU's `spin_unlock` reads `saved_eflags` and conditionally `sti`s, it's restoring the **acquirer's** IF state, not its own.

That's fine in practice because every code path that calls `schedule()` had IF=1 before `spin_lock`'s `cli` — IRQ contexts because of the IRQ stub's `cli` happens AFTER the IRQ already brought us in (and we want IF=1 on return), and direct calls because the kernel runs with IF=1 by default. So the acquirer always pushed IF=1, and the releasing CPU's `sti` is always the right thing to do.

If we ever introduce a code path that calls `schedule()` with IF=0 by design (an NMI-context yield, say — improbable on i386 but conceivable), this would break. Worth a comment in the header.

## The cpu_pin field

When `task_init()` set `g_tasks[0]` (kmain) as `is_idle = 1`, and `task_init_ap_idle` did the same for AP idle, the boot got past the banner... and then froze. The reason: `is_idle` tasks aren't on the round-robin ring (they're picked only as a fallback when `pick_next_ready` returns NULL). With kmain being `is_idle`, the BSP would constantly pick kernel tasks and the demo tasks/reaper from the ring, never falling back to kmain — so kmain never got CPU time after the first preemption, and the boot hung mid-banner.

The fix is conceptually simple but architecturally annoying: kmain should participate in the round-robin like any other task, so it gets a fair share. But if it's on the ring, the AP could pick it — and kmain runs on the BSP's boot stack, which the AP would corrupt.

Solution: a per-task `cpu_pin` field. -1 = any CPU; otherwise pin to the named CPU. Three uses:

- **kmain (g_tasks[0]): `cpu_pin = 0`.** On the ring, gets fair rotation. AP skips it during `pick_next_ready`.
- **AP idle (`task_init_ap_idle`): `cpu_pin = cpu_id`.** Off the ring (`is_idle = 1`), only this CPU's `pick_next_ready` falls back to it.
- **User tasks (`task_create_user`, `task_fork`): `cpu_pin = 0`.** *(Session 33 limitation; see below.)*

The pickability check:

```c
static inline int pickable_by(const struct task *t, int my_cpu) {
    if (t->state != TASK_STATE_READY) return 0;
    if (t->cpu_pin != -1 && t->cpu_pin != my_cpu) return 0;
    return 1;
}
```

With this, the AP traversing the ring naturally skips over kmain, the BSP traverses freely, and AP idle is reachable only via the fallback path on AP.

## Why user tasks stay pinned to BSP

The most ambitious version of this session would have user tasks free to migrate. We tried that. Within ~1 second of boot, three reliable failure modes appeared:

1. **`init: exec failed: httpd.elf`.** `task_exec_inplace` returns -1. The fs/elf load path has shared mutable state (open-file tables, the bcache, the elf-loader's scratch) and two CPUs hammering it concurrently produce torn reads.
2. **`init: stinit: exec failed`.** Two CPUs printing through `kprintf` interleave their characters byte-by-byte. Cosmetic, but indicates that even the printing path isn't atomic.
3. **`Page fault (err=0x0) at 8:1d54b … fault addr (CR2) = 0x00000001`.** A NULL+1 dereference somewhere in the kernel. The PC moved between runs (1d54b, 1f527 in different boots), confirming a race rather than a deterministic bug — the corrupted state happened to land at different addresses each time.

All three trace back to "the syscall surface assumes a single CPU." Fixing them properly is a multi-session project: every shared structure (`g_tasks` ring, fs descriptors, bcache slots, kmalloc free-list, paging PD/PT pages, sock table, pipe table, signal handlers, …) needs either its own lock or atomic operations. Or we add a Big Kernel Lock around `syscall_dispatch` and serialize all syscall work — coarser but correct, and a reasonable bridge until per-subsystem locks land.

For session 33, the pragmatic decision: pin user tasks to BSP. AP scheduling demonstrably works (the LAPIC timer fires, `schedule()` runs on AP, it picks kernel-mode demo tasks off the ring, dispatch counters advance). The all-tests-pass bar is preserved: all 23 selftests run, `curl` works, no crashes. The boring-but-important next session will be "make syscalls SMP-safe" which then naturally lifts the user-task pin.

The pin is a one-line change in `task_create_user` and `task_fork`. To unpin in a future session, set `cpu_pin = -1` and add the locking.

## Per-CPU idle bring-up

The AP entry sequence becomes:

```c
void ap_entry(uint32_t my_id) {
    /* GDT, TSS, IDT, LAPIC enable as before (session 31) */
    ...

    struct task *idle = task_init_ap_idle(my_id, me->kernel_stack_top, me->idle_stack);
    me->idle    = idle;
    me->current = idle;

    lapic_timer_init(10000000u);          /* ≈10ms periodic */

    __atomic_store_n(&me->online, 1, __ATOMIC_RELEASE);

    kprintf("[smp] AP%u online (apic_id=%u, lapic-timer @ vec 0x40)\n", ...);

    for (;;) {
        __asm__ volatile ("sti; hlt");
    }
}
```

The order matters:

1. **Build the idle TCB before enabling the LAPIC timer.** Otherwise the very first timer tick hits `schedule()` with `cpu->current == NULL` and we'd reach the fallback "use g_tasks[0]" path on the AP — bad, that's BSP's kmain, pinned to BSP.
2. **Mark `me->online = 1` AFTER scheduler state is set up.** The BSP's `wait_for_ap_online` polling is the gate the BSP uses before starting any user-visible work. If we marked online before the scheduler was ready, the BSP could spawn `init.elf` while AP is still in a transitional state — reading `me->current == NULL` or `me->idle == NULL` at the wrong moment.

The idle TCB itself is just a plain `struct task` with `is_idle = 1`, `cpu_pin = my_id`, `kernel_stack_top` set to the AP's existing stack (the one `smp_init` allocated and the trampoline parked us on), and not on the ring. `task_init_ap_idle` allocates a free slot from `g_tasks[]`, doesn't add to any list, and returns the pointer.

When the LAPIC timer fires, `schedule()` runs:
- `prev = idle` (`is_idle = 1`, so we skip the "demote to READY" — idle stays "running" on this CPU as the fallback target).
- `pick_next_ready(NULL, my_cpu = 1)` because `prev->is_idle` short-circuits the ring start. The fallback path scans `g_tasks[0..N]` for the first pickable.
- Finds `demo_task_a` or `demo_task_b` (READY, `cpu_pin = -1` default for kernel tasks created via `task_create`).
- `task_switch` to it.

When the kernel task yields (most do via `pit_sleep` → `hlt` → PIT IRQ on the BSP, or LAPIC timer on the AP), `schedule()` demotes it back to READY and the cycle continues. Eventually `pick_next_ready` returns NULL (everyone busy elsewhere), the AP falls back to its idle, and `for (;;) sti; hlt` waits for the next tick.

## SMP_STATS syscall + selftest

```
SYS_SMP_STATS = 53 — (eax=53, ebx=uint32_t out[8]) -> N CPUs
   out[0..3] = LAPIC-timer tick count for cpu 0..3
   out[4..7] = non-idle dispatch count for cpu 0..3
```

Two tick counters per CPU make sense as separate signals:

- **LAPIC-timer ticks** is purely "how many times did the timer fire." On AP, that should be (timer_freq × elapsed_seconds). On BSP, currently 0 because the BSP uses the PIT.
- **Dispatch count** is "how many times did `schedule()` pick a non-idle task on this CPU." If the LAPIC timer ticks are nonzero but dispatches are zero, the CPU is firing timer interrupts but always falling back to idle — meaning no other CPU's tasks are reaching this one (a pin issue, or a dead run-queue).

The `[t22]` selftest snapshots both before/after a 300ms `sys_sleep_ms`, prints deltas:

```
  cpu count: 2, 300ms deltas:
    cpu0: 0 lapic-timer ticks, 36 task dispatches
    cpu1: 30 lapic-timer ticks, 30 task dispatches
  PASS: AP1 dispatched 30 kernel tasks in 300ms
```

`cpu0: 0 lapic-timer ticks` is expected — BSP uses PIT. `cpu0: 36 task dispatches` says BSP did 36 context switches over the 300ms. `cpu1: 30 lapic-timer ticks, 30 dispatches` says AP1 fired its timer 30 times, dispatched 30 times — every tick picked a real task (i.e., AP isn't sitting idle when there's work).

## Bugs and lessons

**1. `cpu_local()` pre-LAPIC-mapping page-fault.** Calling `cpu_local()` before `lapic_init()` page-faulted on `0xFEE00020`. Captured by the `g_smp_ready` flag — `cpu_local()` callers short-circuit to `cpu_at(0)` or `&g_tasks[0]` before the flag flips.

**2. New tasks never released the scheduler lock.** First "fix the deadlock" iteration. Lock-handoff across `task_switch` works for resumed tasks (they unlock at the `spin_unlock` after their own `task_switch`), but a freshly-created task's first scheduling boots straight to its entry function — never reaching the `spin_unlock`. Symptom: kmain hangs mid-banner because all subsequent `schedule()` calls spin forever waiting for the lock that demo_a's entry was supposed to drop. Fix: `_task_entry_trampoline` (asm) calls `_post_switch_finalize` (C, releases lock), then `ret`s into the real entry.

**3. `task_fork` zeroed `cpu_pin` to BSP-pinned by accident.** `memset(child, 0, sizeof(*child))` zeros every field including `cpu_pin`. Default 0 means "pinned to CPU 0". So forked children were unintentionally BSP-only, hiding the user-task migration path entirely. We set `cpu_pin = -1` explicitly after the memset to fix it (then set it back to 0 once we discovered the syscall-surface SMP issues — see bug 4).

**4. The kernel syscall surface isn't SMP-safe.** Once `cpu_pin = -1` worked and AP started picking up user tasks, init's `exec("httpd.elf")` started failing reliably (returning -1 from `task_exec_inplace`), accompanied by NULL-deref page faults at random kernel addresses. fs/bcache/elf/paging-destroy all assume a single CPU is in the kernel at a time. Mitigation for this session: pin user tasks to BSP. Real fix: per-subsystem locks (or BKL). Tracked as the natural follow-up.

## Files touched

- `kernel/task.{h,c}` — Per-CPU current, `g_sched_lock`, `cpu_pin`, `task_init_ap_idle`, `task_smp_ready`, `post_switch_finalize`, `pick_next_ready` with pin awareness, all `g_current` references migrated to `cpu_current()`/`cpu_local()->current`, ring mutations under the lock.
- `kernel/task_switch.S` — `_task_entry_trampoline`, lock-release prologue on `_fork_child_return`.
- `kernel/lapic.{h,c}` — `lapic_timer_init`, `lapic_timer_stop`, three new register offsets.
- `kernel/isr_stubs.S` — `_lapic_timer_stub` + `lapic_irq_common_stub`.
- `kernel/isr.c` — `lapic_irq_handler`, per-CPU tick + dispatch counters.
- `kernel/idt.c` — vector 0x40 → `lapic_timer_stub`.
- `kernel/smp.{h,c}` — `cpu_local.idle`; `ap_entry` builds idle and starts LAPIC timer.
- `kernel/tss.{h,c}` — per-CPU dispatch in `tss_set_kernel_stack`.
- `kernel/syscall.{h,c}` — `SYS_SMP_STATS = 53`.
- `kernel/kernel.c` — `task_smp_ready()` after `smp_init()`.
- `user/libuser.{h,c}` — `sys_smp_stats()` wrapper.
- `user/sh.c` — `[t22]` rewritten.

About 350 LOC net change. Most of it is the scheduler rewrite in task.c and the deep-dive comments justifying the lock-handoff design.
