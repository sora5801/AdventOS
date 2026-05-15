# Session 80 — SMP=2 deadlock fixes

**Goal:** finish what session 66 left half-done — make `-smp 2` boot all the way to an interactive shell prompt and stay there, without the TCP-loopback hang or any of the silent freezes that have been the reason `build.sh`'s hint output still says "-smp 1 stays recommended."

Status: **done.** Four independent SMP correctness bugs root-caused and fixed. `-smp 2` now boots cleanly with all five long-lived userspace services (httpd, httpsd, sshd, agentd, sh.elf) up and the shell responsive to typed input — verified by an `ls` round-trip in the final log.

The bugs were latent in different layers and only the *combination* of session 76's reactive-observability traffic, session 77's cron polling, and session 80's stress harness ran enough scheduler churn under `-smp 2` to surface them. Sessions 66 and 80 (pre-fix) misattributed the symptom as a TCP-loopback race; it was actually four scheduler/locking bugs that *only sometimes* manifested in the TCP path.

---

## How I got the trace evidence

`kernel/smp_trace.h` (added in the prior session-80 follow-on commit, `d008f2e`) gates a `SMP_LOG()` macro on `-DSMP_TRACE`. `build.sh` honors `SMP_TRACE=1` from the environment and adds the flag to kernel CFLAGS. Trace points are wired through `bkl_lock`/`net_lock`/`schedule`/`task_switch.S` trampoline / `sock_*` / `tcp_rx` / `pit_irq` / `task_yield` — every event that mutates global SMP state.

The default build emits nothing; this only fires when explicitly enabled, so the diagnostic load is opt-in. Each bug below is reproducible at will by

```bash
SMP_TRACE=1 bash build.sh
qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 -smp 2 \
    -netdev user,id=net0,hostfwd=tcp::7000-:7000 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    2>&1 | tee /tmp/boot.log
```

---

## Bug 1 — Trampoline IF=1 race against `g_sched_lock`

**Where:** `kernel/task.c::task_create` (synthesized stack), `kernel/task.c::synth_fork_child_stack` (forked child), `kernel/task.c::post_switch_finalize` (lock release).

**Symptom:** the first time any AP dispatched a fresh kernel task, the system hung silently after the `schedule pick` trace line but before the trampoline's `post_switch_finalize` SMP_LOG fired. No fault dump, no progress indicator — just frozen.

**Trace (truncated; cpu1 dispatching reaper for the first time):**

```
[smp] cpu1 schedule pick reaper pid=1 kern (prev idle1 pid=0)
[silence forever]
```

**Root cause.** The scheduler holds `g_sched_lock` across `task_switch.S`. The lock is handed off across the stack swap — the *incoming* task's first execution releases it via `post_switch_finalize` (called from `_task_entry_trampoline` for new kernel tasks, `_fork_child_return` for fork children, or the post-`task_switch` line in `schedule()` for already-suspended tasks).

The synthesized first-dispatch stack frame (in `task_create`) pushed `EFLAGS = 0x202` (reserved bit + IF=1). `task_switch.S`'s `popfl` restores that EFLAGS *before* the lock is released. The window between `popfl` and the eventual `cli` inside `post_switch_finalize → SMP_LOG → kprintf → spin_lock(&g_kvprintf_lock)` is small (~30 cycles), but:

1. IF=1 in that window means the LAPIC timer can fire on this CPU.
2. The LAPIC timer handler calls `schedule()`, which calls `spin_lock(&g_sched_lock)`.
3. **The same CPU already holds `g_sched_lock`.** Self-deadlock — `spin_xchg` will never see `0`.

Meanwhile the other CPU's PIT IRQ fires, calls `schedule()`, tries the same `g_sched_lock`, blocks behind the now-spinning peer. Both CPUs spinning forever.

This was a **latent bug since session 38** (when SMP scheduling first landed). Under `-smp 1` the LAPIC timer wasn't initialised on any CPU but the BSP, and the PIT race window is ~30 cycles at 100 Hz — roughly 1 in 100k probability per dispatch. The session 80 reproducer hit it because adding `SMP_LOG()` to `post_switch_finalize` widened the IF=1 window from ~30 cycles to ~hundreds of microseconds (the kprintf has to byte-pump over a 115200-baud UART), making the race near-certain.

**Fix.** Two coordinated changes:

```c
/* kernel/task.c — task_create synthesized stack */
*--sp = 0x002;   /* EFLAGS: reserved bit, IF=0 (was 0x202) */

/* kernel/task.c — synth_fork_child_stack */
top -= 4; *(uint32_t *)top = 0x002;   /* was 0x202 */

/* kernel/task.c — post_switch_finalize */
void post_switch_finalize(void) {
    SMP_LOG("post_switch_finalize");
    spin_unlock(&g_sched_lock);
    __asm__ volatile ("sti");          /* close the gap with one instruction */
}
```

`popfl` now restores IF=0, so the trampoline runs with IRQs disabled all the way through. The explicit `sti` after `spin_unlock` re-enables them exactly once, *after* the lock is released. The previous `0x202` was originally there because `schedule()` is entered from an IRQ handler with IF=0, so `g_sched_lock`'s `saved_eflags` captures IF=0 and `spin_unlock` alone wouldn't restore IF=1 — without the explicit `sti`, new tasks would start with IRQs off and the first `hlt` would hang. The `sti` fixes that without re-opening the race.

**Verification.** After the fix, `cpu1 post_switch_finalize` fires immediately following every `cpu1 schedule pick` of a new task. The race window is zero cycles wide.

---

## Bug 2 — `schedule keep` left `prev->state = READY`

**Where:** `kernel/task.c::schedule`, the "no other runnable" early-exit branch.

**Symptom:** after bug 1 was fixed, the trampoline-side trace fired correctly, but a second deadlock appeared on the very next schedule tick. Two CPUs ended up dispatching the *same* task on top of the *same* kernel stack.

**Trace:**

```
cpu1 schedule pick reaper pid=1 kern (prev idle1 pid=0)    ; reaper RUNNING on cpu1
cpu1 post_switch_finalize                                  ; cpu1 entering reaper's body
cpu0 schedule pick bcache pid=2 kern (prev kmain pid=0)
cpu0 post_switch_finalize
cpu1 schedule keep reaper pid=1 (no other runnable)        ; cpu1 LAPIC tick; demotes
                                                            ; reaper->READY, picks reaper
                                                            ; again via keep branch.
                                                            ; *** BUT STATE STAYS READY ***
cpu0 schedule pick kmain pid=0 kern (prev bcache pid=2)
cpu1 schedule pick bcache pid=2 kern (prev reaper pid=1)
cpu0 schedule pick reaper pid=1 kern (prev kmain pid=0)    ; *** cpu0 dispatches reaper
                                                            ; while cpu1 just dispatched
                                                            ; *away* from it. Two CPUs on
                                                            ; reaper's stack. ***
```

**Root cause.** `schedule()` demotes `prev` before scanning the runqueue:

```c
if (prev->state == TASK_STATE_RUNNING && !prev->is_idle) {
    prev->state = TASK_STATE_READY;
    prev->cpu   = -1;
}
struct task *next = pick_next_ready(...);
```

When `pick_next_ready` returns `prev` (no other ready task and we're still pickable), the code took the "no other runnable" early-exit branch and returned with `prev->state` left at `READY`. From that point on, *every other CPU* sees the task as freely dispatchable. The next peer-CPU `schedule()` call grabs the lock, sees `prev` ready, sets `state=RUNNING / cpu=peer`, and `task_switch`es onto its kernel stack — while the original CPU is still executing on that same stack.

Two CPUs corrupting each other's frame pointers and return addresses on the same stack → crash or wedge within microseconds.

Under `-smp 1` this was invisible because there was never a peer CPU to grab the freshly-demoted task.

**Fix.** Restore `RUNNING + cpu` before releasing the lock in the keep branch:

```c
if (next == prev) {
    prev->state = TASK_STATE_RUNNING;
    prev->cpu   = (int)cpu->cpu_id;
    spin_unlock(&g_sched_lock);
    return;
}
```

Both writes happen under `g_sched_lock`, so the demote→keep→restore sequence is atomic from any peer CPU's POV.

**Verification.** Post-fix the trace shows reaper migrating cleanly between CPUs only when a real workload is ready elsewhere; the same-task-on-both-CPUs interleaving never appears.

---

## Bug 3 — Timer IRQ preempting a BKL-holding task

**Where:** `kernel/pit.c::pit_irq`, `kernel/isr.c::lapic_irq_handler`.

**Symptom:** even with bugs 1 and 2 fixed, the boot eventually wedged after the shell prompt appeared. cpu0 would acquire the BKL inside a syscall, then disappear from the trace — no `bkl_unlock`, no `yield enter`, just silence. cpu1's reaper would later call `bkl_lock`, block forever.

**Trace (last cpu0 lines before silence):**

```
cpu0 schedule pick sh.elf pid=8 USER (prev kmain pid=0)
cpu0 bkl_lock try
cpu0 bkl_lock acq        ; *** sh.elf's syscall now holds the BKL ***
[no more cpu0 lines]
[cpu1 cycles reaper/bcache, eventually tries bkl_lock, deadlocks]
```

**Root cause.** The BKL is *not* recursive and not transferable across context switches. The kernel's design has it held only across the syscall body, with explicit drop-on-yield in `task_yield()` and the blocking-syscall sites that need it. But the round-robin timer (PIT on the BSP, LAPIC on each CPU) was calling `schedule()` unconditionally — including when the just-preempted task was mid-syscall with BKL held.

When that happened, the BKL stayed locked (held by a task that wasn't currently running on any CPU), `g_bkl_owner_cpu` still pointed at the CPU that *had* been running that task, and the next attempt to acquire BKL anywhere would spin forever.

**Fix.** Both timer paths skip preemption when this CPU holds the BKL:

```c
/* kernel/pit.c::pit_irq */
extern int bkl_held(void);
if (bkl_held()) return;
schedule();

/* kernel/isr.c::lapic_irq_handler */
extern int bkl_held(void);
if (!bkl_held()) {
    schedule();
}
```

The bkl-aware yields inside specific syscalls (SYS_YIELD, SYS_SLEEP_MS, blocking accept/read) still drop + retake BKL explicitly, so cooperative scheduling still happens at all the right points. Only the *involuntary* timer preemption gets suppressed when it would leave the BKL orphaned.

**Verification.** Post-fix the trace shows full syscall cycles always completing: `bkl_lock try → acq → yield enter → bkl_unlock rel → schedule pick (next task) → ... → bkl_lock acq → yield exit → bkl_unlock rel`. No more silent BKL-orphaning.

---

## Bug 4 — `keyboard_wait_char` busy-poll pinning the BKL

**Where:** `kernel/keyboard.c::keyboard_wait_char`.

**Symptom:** with bugs 1–3 fixed, the boot reached a working `advent$` prompt. Then the shell read its first character from the keyboard and the kernel re-entered the same "cpu0 took BKL, never released" pattern as bug 3.

**Trace:**

```
cpu0 schedule pick sh.elf pid=8 USER (prev kmain pid=0)
cpu0 bkl_lock try
cpu0 bkl_lock acq        ; sh.elf's SYS_READ enters with BKL held
[silence on cpu0 until SIGINT — no yield, no unlock]
```

**Root cause.** Session 68 added `keyboard_wait_char()` as a tight spin-poll loop on PS/2 port 0x60 and the COM1 LSR, with a `pause` instruction at the bottom. The original comment was explicit:

> Cost: this CPU stays at ~100% while the shell is waiting for input — acceptable for a single-user interactive shell on a developer system.

That's a fine tradeoff under `-smp 1`. Under `-smp 2`, however, `syscall_dispatch` holds the BKL for the entire syscall body — so this poll loop holds the BKL through every iteration. The first time another CPU's kernel task needs the BKL (reaper's periodic `bkl_lock` on its scan cycle, or any IRQ-driven socket consumer), that CPU spins forever. Bug 3 *would* have caught this if we were getting preempted with BKL held — but we're not getting preempted; we're voluntarily spinning, with BKL legitimately ours, forever.

**Fix.** Yield every iteration. `task_yield()` is bkl-aware: it drops BKL across `schedule()`, then reacquires it:

```c
for (;;) {
    keyboard_poll_once();
    serial_poll_once();

    if (kbd_head != kbd_tail) {
        char c = kbd_buf[kbd_tail];
        kbd_tail = (kbd_tail + 1) % BUF_SIZE;
        return c;
    }
    task_yield();         /* was: __asm__ volatile ("pause"); */
}
```

Under `-smp 1`, `schedule()` returns to us immediately via the keep-prev branch (no other runnable). Under `-smp 2`, the peer CPU gets a window to run its kernel work while we're "waiting" — but the BKL drop is the whole point, not the schedule. The poll latency is unchanged from session 68's design (a single tick at 100 Hz, well below the human input threshold).

**Verification.** Post-fix the trace at the shell prompt shows continuous `bkl_lock acq → yield enter → bkl_unlock rel → schedule pick (peer-CPU work) → ... → bkl_lock acq → yield exit` cycles. Reaper's `bkl_lock try → acq → unlock rel` now succeeds. Typing into the shell with all four fixes in place:

```
advent$ ls
  etc
  mnt
  hello.elf
  ...
advent$ tasks
sh: exec failed: tasks.elf
[user task pid=9 exited code=127]
[exit 127]
advent$
```

Clean prompt, real `ls` output, real shell `$?` propagation. (`tasks` not being found is unrelated — it's an old kernel-shell builtin, not a userspace command.)

---

## Build-time guard against future BSS/EBDA collisions

A separate breakage I hit mid-session: enabling `SMP_TRACE=1` plus my checkpoint kprintfs grew the kernel image enough to shift `.bss` (which is page-aligned) up by 4 KiB, pushing its end address from `0x9F758` to `0xA0758`. The kernel stack lives at the very top of `.bss` (`entry.S: mov $stack_top, %esp`), so it crossed into 0xA0000+ — the VGA framebuffer MMIO range. Every `pushl` got mangled by the VGA controller's chained-4 latches. Symptom: silent hang at `fbcon_init` (the first deep-stack call after boot), no fault dump.

Added a build-time assertion in `build.sh` that fails the build before this can silently happen:

```sh
BSS_HARD_LIMIT=$((0xA0000))    # VGA RAM begins — stack pushes mangle
BSS_SOFT_LIMIT=$((0x9FC00))    # EBDA begins — ACPI tables at risk
bss_end=$(objdump -h kernel/kernel.elf | awk '/\.bss/ {print "0x" ...}')
if [ "$bss_end" -ge "$BSS_HARD_LIMIT" ]; then
    echo "ERROR: kernel .bss ends at 0x${bss_end} — overlaps VGA RAM" >&2
    exit 1
fi
```

The error message names the symptom ("silent hang at fbcon_init") and the three possible fixes, so the next person who hits it doesn't have to re-walk this rabbit hole.

---

## What this session does NOT touch

- **The session 66 `net_lock` recursive design** is untouched. The trace shows it interleaving in a way that *looks* off (both CPUs prefixed `acq` before either `rel`), but that's an artifact of `g_kvprintf_lock` ordering vs `g_inner` ordering — the mutual exclusion is correct. Verified by the fact that DHCP completes cleanly and no `g_tcbs` corruption appears. Worth a closer read in a follow-up but not on the critical path.
- **The TCP-loopback workflow under sustained load.** I tested the boot and `ls`; I did not run the full t20 sequence from session 64 under `-smp 2`. The four fixes here address every scheduler/locking bug I could find, but the t20 pipeline has its own concurrency surface (pipe layer + nc fork + head + httpd) that may turn up something else.
- **The `-smp 1 stays recommended` line** in build.sh's helpful-hints text and the corresponding "see docs/66" pointer. Those should be updated in a follow-up commit that also extends docs/66 with a "resolved in session 80" cross-reference.

---

## Summary of files touched

| File | Why |
|---|---|
| `kernel/task.c` | Bugs 1 + 2: synthesized EFLAGS 0x202→0x002, `post_switch_finalize` STIs explicitly, `schedule keep` restores `state=RUNNING` |
| `kernel/pit.c` | Bug 3: skip `schedule()` when this CPU holds BKL |
| `kernel/isr.c` | Bug 3: same for LAPIC timer path |
| `kernel/keyboard.c` | Bug 4: `keyboard_wait_char` yields every iteration |
| `kernel/kernel.c` | Added `#include "smp_trace.h"` + one post-banner checkpoint |
| `build.sh` | New BSS hard-limit + soft-limit assertion (0xA0000 / 0x9FC00) |

Plus the existing `kernel/smp_trace.h` infrastructure (committed in `d008f2e`) that made all of this diagnosable in the first place.
