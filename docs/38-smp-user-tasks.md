# Session 38 — SMP for user tasks (partial: BKL + race fixes, AP gate behind a flag)

**Goal stated:** finish what session 33 started — let user tasks run on the AP, not just the BSP.

**What landed:** the *infrastructure* needed to do this safely (Big Kernel Lock around the syscall surface, kprintf serialization, BLOCKED-state task creation that prevents the scheduler from picking half-built tasks) plus a *gate* — `g_ap_runs_user` in `kernel/kernel.c`. With the gate set to 0 (default, what ships), user tasks remain BSP-pinned exactly as in session 33; with the gate set to 1, user tasks run anywhere — and **occasionally page-fault** in the fork+exec storm during boot.

The honest framing: session 33 was 70% of the work; session 38 is another 25%; the last 5% (cross-CPU TLB shootdowns, more careful auditing of `paging_clone_user_pd` and `task_exec_inplace` against transient state) is still ahead. The gate makes the partial nature explicit and reproducible: anyone who flips it to 1 sees the same races that motivated session 33's pin in the first place, but now with the locking groundwork that earlier sessions lacked.

End state with the gate at 0 (default) — same numbers as session 33's `[t22]`:

```
[t22] SMP: APs run kernel tasks via LAPIC-timer preemption
  cpu count: 2, 300ms deltas:
    cpu0: 0 lapic-timer ticks, 42 task dispatches
    cpu1: 30 lapic-timer ticks, 30 task dispatches
  PASS: AP1 dispatched 30 kernel tasks in 300ms
  shell pid=8 on CPU apic_id=0 (user tasks pinned to BSP)
```

End state with the gate at 1: init.elf launches cleanly (`[boot] launched init.elf as pid 5`), but `init`'s sequential fork+exec of `httpd.elf` / `httpsd.elf` / `sh.elf` produces an intermittent `init: exec failed: <name>` followed by a user-mode page fault on a libc or user-text page that should be mapped. Repro is non-deterministic — different fault PCs each run, all in user-VA range with `err=0x4` (user-mode read of non-present page). Diagnosed below; fix is left for a future session.

## What's in scope

In:

- **`kernel/bkl.{h,c}`** (new) — Big Kernel Lock. `bkl_lock` / `bkl_unlock` / `bkl_held`. Wraps every entry to the kernel via `int 0x80`. Tracks owner CPU via LAPIC ID for the per-task drop-on-yield logic.
- **`kernel/syscall.c`** — `syscall_dispatch` brackets every case in `bkl_lock` / `bkl_unlock`. SYS_SIGRETURN's early-return path drops the lock too. SYS_WAIT explicitly drops the lock around the blocking `task_wait_current` so other CPUs can do kernel work while we wait.
- **`kernel/task.c`** — `task_yield` and `pit_sleep` now check `bkl_held` and drop/retake the BKL across the yield, so yielding-while-holding-BKL doesn't deadlock other CPUs. `task_create` returns the new task in `TASK_STATE_BLOCKED` rather than `READY` — caller (`task_create_user`, `task_fork`, plus `task_make_runnable` for plain kernel tasks) flips to READY only after all field setup is complete. Closes the "AP picks half-built task" race that bit immediately on cpu_pin lift.
- **`kernel/task.c`** — `task_make_runnable(t)` helper added. All `task_create` callers (`task_reaper_start`, `bcache_start_syncer`, `kmain`'s demo_a/demo_b, `shell.c`'s mtxa/mtxb) now wrap `task_create` in `task_make_runnable(...)`. `task_create_user` and `task_fork` finish their setup before calling it.
- **`kernel/bcache.c`** — `syncer_task` (kernel task with `cpu_pin = -1`, runs on AP) now takes BKL around `bcache_sync()`. Without this, BSP's `fs_open` mid-syscall would race the syncer's bcache walks.
- **`kernel/task.c`** — `task_reaper` likewise takes BKL around its scan-and-free pass, since it touches `paging_destroy_user_pd` and the per-task `cr3` field that user-side `exec` may also be modifying.
- **`kernel/kprintf.c`** — `g_kvprintf_lock` wraps `kvprintf` and `kputs`; `g_kputc_lock` wraps the per-char three-sink fanout. Without this, two CPUs printing concurrently produce char-by-char interleaved output ("init: stinit: started" instead of "init: started" + "init: started").
- **`kernel/kernel.c`** — `g_ap_runs_user` flag (0 by default). The kmain init.elf launch is BKL-bracketed because kmain is not in syscall context but does touch fs / elf / paging that other (BKL-protected) tasks are also touching.
- **`kernel/fs.h`** + **`build.sh`** — `FS_DISK_OFFSET_SECTORS` bumped from 200 → 256 because `kernel.bin` grew past LBA 200 with all the new locking code, pushing the FS image to the wrong offset.

Out:

- **The actual user-task migration.** With `g_ap_runs_user = 1`, the system boots cleanly to the banner but then hits intermittent page faults during init's spawn-services storm. Diagnostics suggest a race in `paging_clone_user_pd` / `task_exec_inplace` where some user-VA page (libc text, init text, stack tail) is briefly invisible to the AP. Plausible culprits: AP-side TLB stale on a cross-CPU page-table mutation, or a window inside `task_exec_inplace` where `t->cr3` is updated before `write_cr3` runs and an AP picks the task between those two writes. Documented under "what's left" — not fixed.
- **Cross-CPU TLB shootdowns.** Real OSes IPI other CPUs to `invlpg` when one CPU mutates a shared PTE. We don't. With user tasks BSP-pinned (default), this doesn't bite because each task only ever runs on one CPU and `write_cr3` flushes that CPU's TLB. With user tasks unpinned, shared PT modifications across CPUs need shootdowns.
- **Per-subsystem locking.** The BKL is intentionally coarse — every syscall body runs single-CPU. A real Linux-shaped kernel would have per-subsystem locks (rwlock for fs metadata, dedicated lock for bcache, per-socket locks, etc.). We don't bother for our 16-task system.
- **`mtest` / `[t22]` updates.** With the gate off, t22 shows the same numbers as session 33. We don't add a new selftest because the migration doesn't reliably work yet.

## Architecture

```
                  USER PROCESS (any CPU when gate=1, BSP only when gate=0)
                  ───────────────────
   user_code()
     int $0x80
        ↓
                  KERNEL (whichever CPU caught the trap)
                  ───────
   syscall_dispatch:
     bkl_lock()                 ← serializes ALL syscall work across CPUs
        switch (num) {
            case SYS_WAIT:
                bkl_unlock();   ← drop before block
                task_wait_current();    /* may schedule away */
                bkl_lock();     ← retake on wake
                ...
            case SYS_SLEEP_MS:
                pit_sleep(a);   /* internally drops/retakes BKL across hlt */
                ...
            case SYS_YIELD:
                task_yield();   /* internally drops/retakes BKL across schedule */
                ...
            (other cases: bkl held throughout)
        }
     signal_check_and_deliver();
     bkl_unlock()


                  KERNEL TASKS (cpu_pin = -1, may run on AP)
                  ─────────────
   bcache syncer:                              task_reaper:
     pit_sleep(5000);                            pit_sleep(200);
     bkl_lock();      ← serialize with           bkl_lock();      ← serialize
     bcache_sync();    user-side fs/bcache       scan + free dead;  with exec/free
     bkl_unlock();                               bkl_unlock();


                  TASK CREATION (race-fixed)
                  ──────────────
   task_create:                                task_create_user / task_fork:
     find slot                                   call task_create
     mark BLOCKED ←  pick_next_ready              ... fill cr3, eip, esp, fds, etc.
     splice into ring                             task_make_runnable(t)
     return t                                       └→ flip BLOCKED → READY
        ↑
     plain kernel callers
     follow with task_make_runnable(...)
```

## What broke and why

### 1. Kernel growth pushed FS off-LBA

`kernel.bin` grew from ~95 KiB (session 37) to 102 KiB after adding `bkl.c` + new locking calls. `FS_DISK_OFFSET_SECTORS` was hardcoded to 200 (= byte 102400) but `boot.bin + kernel.bin = 103088` bytes — fs.img landed past sector 200. Symptom: `fs: bad magic in superblock`. Fix: bumped to sector 256 in both `kernel/fs.h` and `build.sh`.

### 2. kprintf interleaving

Two CPUs calling `kprintf` concurrently produced byte-by-byte interleaved output. The per-char serial port write is hardware-atomic but adjacent characters from different CPUs would mix:

```
init: stinit: exec failed: httpd.elf
arted httpd.elf as pid 6 (once)
```

Fix: a higher-level `g_kvprintf_lock` around `kvprintf` and `kputs`, plus a per-char `g_kputc_lock` for direct `kputc` calls (e.g., from SYS_WRITE). Two locks rather than one because `kvprintf` calls `kputc` inside its loop — recursive lock would simplify but spinlock isn't recursive.

### 3. The half-built task race

When session 33's `cpu_pin = 0` came off, the very first init.elf launch faulted at user EIP `0x40000000` — the very first byte of init's text — with `err=0x4` (user mode, page not present).

Diagnosis: `task_create` set state to `TASK_STATE_READY` *before* `task_create_user` had set `cr3` / `user_eip` / `user_esp`. AP idle's `pick_next_ready` scans all `g_tasks` looking for READY tasks (since AP idle has no ring anchor). It found the half-built init, set up to dispatch, did `write_cr3(t->cr3 = 0)` (still zero from `memset`), and tried to iret to `t->user_eip = 0`. Page fault on the very first instruction.

Fix: `task_create` now leaves the task in `TASK_STATE_BLOCKED`. `pick_next_ready` skips BLOCKED. The caller (`task_create_user`, `task_fork`) finishes field setup first, then calls `task_make_runnable(t)` which flips state to READY under `g_sched_lock`. Plain kernel-task callers (demo_a, reaper, syncer) wrap `task_create` in `task_make_runnable(...)` since they don't need the customization window.

This fix landed and is correct; without it, even BSP-pinned mode would have raced internally. It just didn't matter pre-session-38 because the BSP-pin meant task_create_user always ran on the only CPU that could pick the new task.

### 4. The drop-on-yield deadlock

First attempt at BKL: hold the lock across `task_yield` and `pit_sleep` via the same lock-handoff trick `g_sched_lock` uses (when this task resumes, the resuming task's `bkl_unlock` releases the lock).

Symptom: kmain hung mid-banner. Diagnosis:

- kmain holds BKL via the explicit `bkl_lock` around the LAUNCH macro.
- kmain prints banner (no yields). PIT IRQ fires. Schedule preempts kmain to demo_a.
- demo_a runs (no BKL needed for its serial writes). PIT fires. Schedule preempts to reaper.
- reaper calls `bkl_lock`. BLOCKS — kmain holds BKL but isn't running.
- reaper spins inside `spin_lock` with IF=0 (cli'd). PIT can't fire on BSP. BSP is stuck.
- AP can fire LAPIC, runs `schedule`, picks any ready task EXCEPT kmain (BSP-pinned). All other ready tasks are non-BKL or want BKL — same fate.

Lock-handoff works for `g_sched_lock` because every code path that takes it (= every `schedule` call) also has a matching `spin_unlock` in its caller's tail. BKL acquirers are all in `syscall_dispatch` — so non-syscall kernel code (kmain, kernel tasks) breaks the invariant.

Fix: drop BKL on yield, with the trade-off documented (in-flight kernel state may be touched by other CPUs while we sleep). For our subsystems — fs/bcache/elf — we don't yield mid-operation, so this is safe in practice. For longer waits (sleep_ms), other CPUs are productive instead of paused.

### 5. The remaining race (NOT fixed)

With the BKL machinery in place and `g_ap_runs_user = 1`, init.elf launches and starts spawning services. The first fork+exec usually succeeds. The second or third intermittently fails with:

```
init: exec failed: httpsd.elf
[!] CPU EXCEPTION 14: Page fault (err=0x4) at 1b:70000a75  CR2 = 0x70000a75
```

The fault PC is in user-VA (libc text, init text, or just past USER_STACK_VA). `err=0x4` means user-mode, read, page-not-present. The CR3 the user task is using doesn't have that page mapped.

Three plausible causes, none fully nailed down:

1. **TLB stale across CPUs.** `paging_clone_user_pd` runs on BSP (with BKL); the new child PD has libc fully mapped. AP picks the child via timer; `write_cr3(child_pd)` flushes AP's TLB, so AP starts with a clean view. But if BSP later modifies the child's PD (e.g., `paging_destroy_user_pd` from reaper, or `sys_brk` from another task that happens to grab a freed PT page), AP's TLB doesn't get invalidated. AP continues using stale "page not present" caching.

2. **`task_exec_inplace` window.** Between `t->cr3 = lr.cr3` (line N) and `write_cr3(lr.cr3)` (line N+5), the task's `cr3` field shows the new PD but the running CPU's CR3 register still holds the old PD. If a PIT IRQ preempts the task here and the AP picks it up, AP sees `t->cr3 = new`, does `write_cr3(new)`, and resumes — but the kernel stack saved EIP is in the OLD PD's address space (which AP doesn't have). AP faults on the resumed instruction.

3. **`paging_clone_user_pd` partial visibility.** The clone allocates ~16 PT pages and ~50+ data pages. If the WRITES TO THE NEW PD happen out-of-order with respect to the WRITE TO `t->cr3`, an AP that reads `t->cr3` might see the new value but the corresponding PT entries might not have propagated. x86 strong memory ordering should rule this out, but our cross-CPU TLB picture is fragile enough that the symptom matches.

The fix likely needs two things: (a) cross-CPU TLB shootdowns when any PD mutation happens (LAPIC IPI to send `invlpg` instructions), and (b) careful re-audit of `task_exec_inplace` to ensure the task is BLOCKED-state during the t->cr3 / write_cr3 window. Probably also needs (c): the FORK path's `paging_clone_user_pd` should issue a memory barrier (`mfence`) before the BLOCKED→READY transition.

Each is a separate session's work. The gate is documented + flippable so a future session can attack it incrementally.

## Selftest pass with gate=0

```
[t22] SMP: APs run kernel tasks via LAPIC-timer preemption
  cpu count: 2, 300ms deltas:
    cpu0: 0 lapic-timer ticks, 42 task dispatches
    cpu1: 30 lapic-timer ticks, 30 task dispatches
  PASS: AP1 dispatched 30 kernel tasks in 300ms
```

All 27 selftests run, no page faults, `curl http://localhost:8601/` continues to serve `httpd.elf`. The BKL serialization adds <1% overhead in steady-state (one extra spin_lock+unlock per syscall, both uncontended outside heavy fork storms).

## Files touched

- `kernel/bkl.h`, `kernel/bkl.c` (new) — Big Kernel Lock + per-CPU owner tracking
- `kernel/kprintf.c` — `g_kvprintf_lock` + `g_kputc_lock`
- `kernel/syscall.c` — `bkl_lock`/`bkl_unlock` brackets + SYS_WAIT explicit drop + SYS_SIGRETURN early-unlock
- `kernel/task.c` — `task_make_runnable`, BLOCKED-state task_create, BLOCKED-then-READY-on-splice in task_fork, `task_yield`/`task_reaper` BKL awareness, `g_ap_runs_user` gate honored in `task_create_user` / `task_fork`
- `kernel/task.h` — `task_make_runnable` exported
- `kernel/pit.c` — `pit_sleep` BKL drop/retake
- `kernel/bcache.c` — syncer holds BKL across `bcache_sync`
- `kernel/kernel.c` — `g_ap_runs_user` flag (defaults 0); LAUNCH macro now BKL-protected
- `kernel/fs.h` — `FS_DISK_OFFSET_SECTORS` 200 → 256
- `build.sh` — `fs_lba=256` to match
- `docs/38-smp-user-tasks.md` — this document

About 250 LOC of new + modified code. The unfinished part is documented as the explicit `g_ap_runs_user` flag — flipping it to 1 reproduces the race for whoever picks up the work next.
