# Session 66 — SMP TCP-loopback race fix (partial)

**Goal:** close the SMP race that session 64 surfaced when agentd joined sshd/httpd/httpsd as a fourth permanent loopback listener. Under `-smp 2` the t20 networking sequence (`nc localhost 80 | head -1`) reliably deadlocks after the four listeners are scanning `g_tcbs` while a SYN flows through `try_loopback`. Session 64's note set `-smp 1` as the recommended config; this session was meant to restore `-smp 2`.

Status: **partial.** A real race was identified and closed (rtl8139 RX IRQ vs syscall-side `g_tcbs` / `g_socks` mutations). Selftest under `-smp 1` stays green at **144 PASS, 0 FAIL** (up from 134 in `d6aa962`). The original `-smp 2` t20 hang still reproduces with agentd in inittab, which means the deadlock has a second source the lock added here doesn't reach. `-smp 1` remains the recommended config in `build.sh`. The next session takes it the rest of the way.

---

## What I expected to find

The session 64 doc described the symptom this way:

> With `-smp 2`, the t20 networking sequence (`nc localhost 80 | head -1`) reliably deadlocks once four listeners are scanning `g_tcbs` while a SYN is being delivered through `try_loopback`'s CLI-protected dispatch. The CLI only serializes against IRQ on the same CPU — a peer CPU running a syscall can still mutate `g_tcbs` mid-scan.

This matches a textbook unprotected-shared-state SMP race. The fix would be a lock around `g_tcbs` mutations.

## What I actually found

Two things were going on under the same surface symptom:

### Real race #1 — rtl8139 IRQ vs syscall, on `g_tcbs` / `g_socks`

The BKL serialises kernel work between *syscalls* (kernel/bkl.c is taken at `syscall_dispatch` entry and dropped only around blocking yields). But the rtl8139 IRQ path

```
rtl_irq → net_rx_frame → eth_rx → ip_rx → tcp_rx
                                       → on_recv / on_connect / on_close
```

runs outside the BKL — interrupt context can't take a spinlock that may be held by a task that isn't currently running. Real-NIC traffic arriving on the BSP can race a syscall on the AP that's mid-write to the same TCB. The `try_loopback` CLI in `ip.c` was the original guard, and it serialises against IRQ on the *same* CPU. It does nothing about a peer CPU.

This race exists regardless of agentd. Session 64's commentary about "four listeners widened the scan window" was right about the trigger but framed it as a syscall-vs-syscall race, which BKL already handles.

### Real race #2 — the t20 hang itself

Reproducing under `-smp 2`:

- without agentd in inittab: t20 (`nc localhost 80 | head -1`) completes, selftest finishes (with two expected `[t47/t48] could not connect` fails since agentd isn't running).
- with agentd in inittab: t20 hangs after `nc localhost 80 | head -1:` prints, two TCBs alloc'd (`slot 7` for nc's client, `slot 8` for httpd's spawned conn), then silence.

Both TCBs are allocated — the SYN flowed through `try_loopback`, the listener spawned a conn TCB and sent SYN-ACK, the SYN-ACK transitioned nc's client to ESTABLISHED and fired `on_connect`, the ACK back transitioned httpd's spawned conn to ESTABLISHED and pushed it onto the listener's accept queue. So the 3-way handshake completed. After that point, no progress: no `[user task pid=N exited]` lines from nc or head, no further trace output for 10+ minutes, but both QEMU processes alive with stable RSS.

What that means: the connection is established, both sides are blocked, and nobody wakes them up. This isn't a `g_tcbs` corruption (the TCB allocations are clean and not exhausted). It's a scheduler/wake-up issue downstream of the connection.

The fix in this session addresses race #1 but not race #2. I haven't pinpointed #2 yet. Diagnostics ruled out:

- **TCB exhaustion** — `tcb_alloc` instrumentation shows only 8 slots used by t20, of 24 available.
- **State machine corruption** — the post-fix sequence is the same as pre-fix, both under `-smp 1` and pre-hang under `-smp 2`. on_connect / state transitions complete normally.
- **Volatile staleness on sock state** — making `state` volatile in `struct sock` didn't change the hang shape (compiler had been optimising correctly through the existing yield-induced barriers).
- **The lock holding through `task_yield`** — already broken correctly. `sock_read` / `sock_accept` / `sock_connect` drop `net_lock` around their yield. With the lock held through yield I'd see immediate deadlock, not 10-minute silence.

Likely candidates for race #2 worth chasing in a follow-up:

- **BKL bouncing under listener-spinwait pressure.** Four listeners in `sock_accept`'s yield loop continuously cycle `bkl_lock → net_lock → q_empty check → net_unlock → task_yield (bkl_unlock + schedule + bkl_lock)`. Each cycle is ~6 atomic ops on shared cache lines. With two CPUs each doing this for two listeners apiece, the per-cycle wait grows in a way the post-fork nc + head + httpd tasks may never get a turn at.
- **Round-robin scheduler skipping a class of task.** `pick_next_ready` walks the run queue but the chain insertion order (`splice_into_ring`) might leave nc-internal-fork or head consistently behind agentd / sshd / httpsd / httpd / sshd — i.e., starvation, not deadlock.
- **A real third producer-consumer race** in the pipe layer or the kbd ring, exposed only when the test pipeline's tasks straddle CPUs.

Each of these is at least one full session to diagnose properly. I'm shipping incremental progress rather than holding the session open until everything's perfect.

## The lock that did land — `g_net_lock`

`kernel/netlock.{h,c}` — a recursive spinlock that protects `g_tcbs` (in `kernel/tcp.c`) and `g_socks` (in `kernel/sock.c`).

```c
static spinlock_t g_inner = SPINLOCK_INIT;
static volatile int g_owner_cpu = -1;
static volatile int g_depth     = 0;

void net_lock(void) {
    int me = my_cpu();
    if (g_owner_cpu == me) {        /* we already own it — bump depth */
        g_depth++;
        return;
    }
    spin_lock(&g_inner);            /* CLIs locally + cross-CPU spin */
    g_owner_cpu = me;
    g_depth     = 1;
}

void net_unlock(void) {
    if (--g_depth > 0) return;
    g_owner_cpu = -1;
    spin_unlock(&g_inner);
}
```

Recursive because `try_loopback` recurses through `tcp_rx` → `spawn_conn_from_listener` → `tcp_send_seg_seq` → `ip_send` → `try_loopback` → `tcp_rx`, three or four levels deep for a TCP handshake. A non-recursive lock at the outermost entry would deadlock the first recursive re-acquire.

### Where it gets taken

| Site                           | Why                                                                |
| ------------------------------ | ------------------------------------------------------------------ |
| `ip_rx` (around `tcp_rx`/`udp_rx`) | IRQ entry, serialises with syscall-side state mutators             |
| `try_loopback`                 | Syscall entry, also covers the recursive re-entry                  |
| `udp_listen`                   | `g_listeners[]` mutator                                            |
| `sock_create` / `sock_bind` / `sock_listen` / `sock_close` / `sock_inc_ref` | one-shot `g_socks[]` mutators       |
| `sock_accept` / `sock_connect` / `sock_read` | block-on-condition; drop lock around `task_yield`         |
| `sock_write`                   | calls into `tcp_send`, needs the same lock                         |
| `sock_accept_avail` / `sock_read_avail` | read-only peeks, lock for consistent reads                |

The drop-around-yield pattern:

```c
while (q_empty(&g_socks[idx])) {
    net_unlock();
    task_yield();
    net_lock();
    if (g_socks[idx].state != SOCK_LISTEN) { net_unlock(); return -1; }
}
```

Mirrors `task_yield`'s own BKL handoff. Without it, the lock would be held through `schedule()`, peer CPUs spinning on `net_lock` would never get it, and the `on_connect` that's *supposed* to wake us up (running on a peer CPU after a SYN-ACK lands) couldn't run either. Standard pattern.

### Lock ordering

`BKL > net_lock`. The BKL is acquired first in `syscall_dispatch` (with IF=1 after the `sti`), `net_lock` is acquired later inside the syscall body (with IF=0 via the inner spinlock's CLI). IRQ-driven entries to `ip_rx` take `net_lock` directly without holding BKL — that's the whole point, since IRQs cannot acquire BKL safely (same-CPU IRQ would deadlock against its own outer syscall).

### Why option (b), not option (a)

The session brief offered two shapes. Option (a) was "take the BKL across `try_loopback`." That doesn't work: the BKL is *already* taken across `try_loopback` because the only legal path into `try_loopback` is via a syscall that has the BKL. The actual gap was the IRQ side, where the BKL cannot be safely taken. So option (b) — a dedicated lock that *can* be acquired from IRQ context — is the only one that closes the gap. Option (a) would have been a no-op.

### Volatile `struct sock.state`

Made `state` `volatile`. The wait loops in `sock_connect` / `sock_accept` / `sock_read` are textbook compiler-CSE bait — read a non-volatile field in a `while`, the compiler may hoist the load into a register and never re-read across the body. The yield in the middle is a function call, which is a memory barrier in practice, but relying on that is fragile. Made the change preemptively; it didn't change the t20 hang behaviour but it's correct.

## What I'd try first in the follow-up

Order of cheapest experiments to most invasive:

1. **Trace what task is being scheduled when the hang sets in.** Add a wraparound buffer of `[cpu, current_task->id]` snapshots at every `schedule()` call, dump the last ~256 on a kernel keypress or a backtrace from QEMU's monitor. If one of the four listeners is on the run queue every tick and nc / head / httpd is never picked, it's starvation in `pick_next_ready`.

2. **Cheap test: reduce listener pressure.** Comment out three of the four listeners (keep just agentd, drop httpd/httpsd/sshd from inittab). If t20's pipeline still hangs at `nc localhost 80 | head -1`, the problem isn't 4-listener BKL pressure. If it suddenly works, it's a scheduler-fairness or BKL-contention issue.

3. **Tighten the listener spin loop.** Replace `task_yield()` in `sock_accept`'s loop with `pit_sleep_ms(1)` so listeners don't get re-dispatched every single timer tick. Same fix httpd's accept loop arguably already wants for power.

4. **Make `sock_accept` block on a condition variable.** `on_connect` signals the queue's "non-empty" cv; `sock_accept` waits on it. That eliminates the busy-yield entirely. The kernel already has `task_make_runnable` / `task_block`; it's a matter of wiring sock to use them rather than spinning.

If I had to bet, it's #1 → #4. The four daemons all sitting in busy-yield loops gives the scheduler nothing to break ties with except round-robin, and round-robin under SMP with two CPUs both round-robining independently is exactly the shape where one logical task can fall off the back.

## Touched files

- `kernel/netlock.{h,c}` — new files, ~80 LOC. Recursive spinlock + the two-line accessor.
- `kernel/ip.c` — `try_loopback` calls `net_lock` around the `tcp_rx`/`udp_rx` dispatch (replacing the old bare cli/popfl). `ip_rx` takes the lock around `tcp_rx`/`udp_rx` only (icmp_rx is left alone, no shared state there).
- `kernel/sock.c` — every public `sock_*` function takes the lock; `sock_accept` / `sock_connect` / `sock_read` use the drop-around-yield pattern.
- `kernel/sock.h` — `state` is now `volatile`.
- `kernel/udp.c` — `udp_listen` takes the lock so its `g_listeners[]` writes serialise with IRQ-side `udp_rx`.
- `build.sh` — kept `-smp 1` as the recommended config (was set in session 64) with an updated comment pointing at this doc.

Selftest: **144 PASS, 0 FAIL** under `-smp 1`. Under `-smp 2` the t20 hang reproduces — net_lock closed one race, didn't close the other.

## What stays out of scope

- The actual t20-under-smp2 fix. Documented as a known followup with concrete next steps above.
- Per-TCB or per-sock locking. The single `g_net_lock` is coarse but the syscall frequency on AdventOS makes its overhead invisible; per-resource locking is a future optimisation, not a correctness need.
- Lock-free TCP. Would require either RCU-grade primitives or a completely redesigned state machine.
- Replacing the BKL outright. Linux-style fine-grained locking is a multi-session refactor; this session adds one more lock at a sensible boundary.

---

# Session 80 update — reproducer + regression

The session-66 deferral got revisited as session 80. Result: not a
fix, an updated diagnosis. The `-smp 2` symptom changed sometime
between session 66 and session 80 — what used to be a *hang* after
the 3-way handshake is now a *kernel crash* before any networking
even reaches user code. Documenting so the next session has a clean
starting point.

## Reproducer

`user/smp-hammer.c` — in-guest binary that hammers agentd's `time`
JSON-RPC method over fresh TCP connections in a tight loop. Counts
successful responses, monitors progress, emits
`DEADLOCK-OBSERVED` + a per-CPU tick-count dump from `sys_smp_stats`
on stall.

```
advent$ smp-hammer 1000          # fire 1000 back-to-back time calls
[smp-hammer] firing 1000 `time` requests to 127.0.0.1:7000
[smp-hammer] done: 1000/1000 ok, 0 fail, 41 s wall      # under -smp 1
```

Under `-smp 2` the binary never gets to run. The kernel crashes
before the shell prompt is interactive, with:

```
[!] CPU EXCEPTION 14: Page fault (err=0x0) at 8:1f898  eflags=0x10016
    fault addr (CR2) = 0x0000007d
    cause = page not present, read, supervisor mode
System halted.
```

This happens AFTER `[boot] launched init.elf as pid 3`, i.e., once
ring-3 tasks start running. The crash is reproducible on every
boot with `-smp 2` and the current kernel.

## What `EIP=0x1f898 / CR2=0x7d` tells us

`0x1f898` is +12 bytes into `signal_check_and_deliver()` (see
`nm kernel/kernel.elf | sort | grep _signal_check`). The function
runs on the IRQ-exit / syscall-exit path of every ring-3 task. Its
first three statements:

```c
if ((r->cs & 0x3) != 3) return;        /* skip kernel-mode tasks */
struct task *t = task_current();
if (!t || !t->is_user) return;
```

`CR2 = 0x7d (125)` is too small to be a real address; it's a NULL-
ish pointer plus a field offset. With session-79's
`TASK_MAX_FDS = 24` (and the struct-task growth from sessions 70+71
sandbox / limits), `t->fds[2].offset` lives near task-struct
offset 124 (0x7c) — within one byte of the observed CR2. So the
fault is consistent with `task_current()` returning a small invalid
pointer (likely `1`, possibly a bit-shifted `g_smp_ready` or some
similarly malformed value) and a subsequent field load missing
the page.

`task_current()` walks `cpu_local()->current`. The plausible
sources of a tiny-but-non-NULL `current` are:

* `cpu_local()` returning a `struct cpu_local` whose `current`
  field was clobbered by a peer CPU mid-`schedule()` — `schedule()`
  holds `g_sched_lock` across the write but the LOAD in
  `task_current()` doesn't acquire any lock.
* The AP being preempted by an IRQ between `cpu->current = next`
  and the actual task switch in `task_switch.S`, leaving
  `current` pointing at a stale TCB.
* A pointer-truncation bug somewhere in the AP-side TSS / cpu_local
  setup that's only exposed once the AP actually starts dispatching
  user tasks (which requires `g_ap_runs_user = 1` — currently
  hard-coded to 0, so this path SHOULDN'T be reachable… unless
  something else has started running user code on the AP).

The third bullet is suspicious because `g_ap_runs_user = 0` is
supposed to keep user tasks pinned to the BSP. If the AP is
actually picking up a user task anyway, that's where the chain
breaks. The AP runs its idle task with `is_idle = 1`, which would
make the schedule() write `cpu->current = next` (next being some
user task) — but `pickable_by()` should be rejecting user tasks
on the AP. Verify the pin check actually runs in the AP's
schedule() path.

## Hypotheses — status

| # | Hypothesis (session-80 spec section C) | Result |
|---|---|---|
| 1 | Missing `g_net_lock` acquisition site | Out of scope — crash happens before networking |
| 2 | BKL + g_net_lock cycle | Same — crash precedes |
| 3 | `sched_lock` + `g_net_lock` cycle | Same — crash precedes |
| 4 | Missed wakeup post-handshake | The original session-66 symptom. Now masked by (5). |
| 5 | New: AP-side task_current() returns invalid pointer | **Likely**. CR2 / EIP / call site all consistent. |

The session-66 hang and the session-80 crash may be related (both
boil down to "the AP touches state it shouldn't") or independent
(crash got introduced separately by struct-task layout growth in
70-79 while the hang stayed dormant). The next session needs to
fix (5) first; (4) becomes diagnosable again once -smp 2 boots.

## Status of the original (session 66) hypothesis bets

Session 66 bet on starvation in `pick_next_ready` (bullet 1) and
busy-yield in `sock_accept` (bullet 4). Those bets stay open: the
session-80 reproducer can't reach them because of the crash.

## Recommended next-session scope

1. **Reproduce (1 hour).** `bash build.sh && qemu ... -smp 2` →
   page fault on boot. Save the boot log to disk.
2. **Pin the offset (1 hour).** `objdump -d kernel/kernel.elf |
   grep -A30 signal_check_and_deliver` — read the disassembly to
   confirm which field at byte 0x7c-0x7d is being loaded. The
   compiler may have inlined something from the function's prologue
   that the source-level reading doesn't capture.
3. **Verify the AP's `pick_next_ready` rejects user tasks.** Add a
   one-line trace at the top of `pickable_by` that prints
   `[pick] cpu=N pid=M state=S pin=P` when state==READY and the pin
   admits this CPU. On `-smp 2`, look for an AP picking a user pid.
4. **Audit the `cpu_local()` chain on AP.** `lapic_id()` →
   `g_cpus[lapic_to_idx[lapic_id]]`. If the lapic-to-idx table
   isn't atomic across `smp_init`, an AP that observes a partial
   table sees an unexpected entry.
5. **Once `-smp 2` boots clean**, run `smp-hammer 5000` and
   re-evaluate the session-66 hang. With current understanding
   it's probably *still there*, masked by (5).

## Workaround

`-smp 1` remains the recommended config (build.sh boot hints). The
crash is total — there is no current way to run `-smp 2` to any
useful endpoint, including testing the session-78 selftests.

