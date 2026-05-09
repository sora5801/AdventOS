# Session 20 — Job control: process groups, sessions, SIGSTOP/SIGCONT

**Goal:** Add the Unix job-control machinery on top of the signal layer from session 16. Concretely: per-task `pgid` and `sid` fields, a new `TASK_STATE_STOPPED` that the scheduler skips, `SIGSTOP` / `SIGCONT` / `SIGTSTP` with the right default actions, the seven POSIX syscalls (`setpgid` / `getpgid` / `setsid` / `getsid` / `killpg` / `tcsetpgrp` / `tcgetpgrp`), and shell-side support for `&` background pipelines + a `jobs` builtin. Pipelines now form their own process groups via `setpgid` — the canonical Unix shell discipline.

End state — two new selftests at boot:

```
[t10] job control: SIGSTOP / SIGCONT / SIGTERM
  [child 17] tick 1
  [parent] -> SIGSTOP 17
  [parent] (paused 80ms; child should not have ticked)
  [parent] -> SIGCONT 17
  [child 17] tick 2
  [parent] -> SIGTERM 17
[sig] pid=17 terminated by signal 15
  child reaped: exit=143  (= 128 + 15 = SIGTERM)

[t11] killpg broadcast to a 2-task pgrp
  shell sid=5 pgid=5
    [c1 pid=18 pgid=18] tick 1
  pgrp 18 has pid1=18 pid2=19
    [c2 pid=19 pgid=18] tick 1
    [c1 pid=18 pgid=18] tick 2
  -> killpg(18, SIGTERM)
[sig] pid=19 terminated by signal 15
[sig] pid=18 terminated by signal 15
  both reaped: c1 exit=143  c2 exit=143  (both = 128+SIGTERM=143)
```

The shell becomes a session leader at startup (`setsid()` → `sid = pgid = pid`). The first `&`-suffixed pipeline you type would print `[1] <pgid>` and return to the prompt. `httpd.elf` keeps serving curl on :80 throughout.

## What's in scope

In:
- `pgid` and `sid` fields on `struct task`; inherited verbatim by `fork`, preserved across `exec`, default to `0` for kernel tasks
- `TASK_STATE_STOPPED` — new state; `schedule()` skips it, `task_state_name` returns `"STOP"`
- `SIGCONT = 18`, `SIGSTOP = 19`, `SIGTSTP = 20` with POSIX-correct default actions:
  - `SIGSTOP` / `SIGTSTP`: `ACT_STOP` (set state to STOPPED, yield)
  - `SIGCONT`: `ACT_CONT` (no-op past the wakeup)
  - the rest: `ACT_TERM` / `ACT_IGN` as before
- `SIGSTOP` is uncatchable (sigaction returns -1), same as `SIGKILL`
- `signal_send` wakes a STOPPED task on `SIGCONT` + clears reciprocal pendings (CONT clears any STOP/TSTP, STOP/TSTP clears any CONT)
- `signal_send_pgrp(pgid, sig)` broadcasts to every live task with matching pgid
- Seven syscalls: `SYS_SETPGID = 32`, `SYS_GETPGID = 33`, `SYS_SETSID = 34`, `SYS_GETSID = 35`, `SYS_KILLPG = 36`, `SYS_TCSETPGRP = 37`, `SYS_TCGETPGRP = 38`
- libuser POSIX-named wrappers (`setpgid`, `getpgid`, `setsid`, `getsid`, `killpg`, `tcsetpgrp`, `tcgetpgrp`) + signal-number defines for `SIGCONT` / `SIGSTOP` / `SIGTSTP`
- Foreground-pgrp slot in `tty.c` (`g_fg_pgid`); `tcsetpgrp` writes it, `tcgetpgrp` reads it
- Shell startup: `setsid()` + `tcsetpgrp(0, getpgid(0))` so the shell owns its own session and the tty's foreground pgrp
- Shell pipelines call `setpgid(0, leader)` in each child + parent-side `setpgid(child, leader)` for fork-vs-exec race safety
- Foreground pipelines do `tcsetpgrp(0, pgleader)` before waiting and `tcsetpgrp(0, getpgid(0))` after
- `&` parsing in tokenize / parse_pipeline; background pipelines skip wait + add to `g_jobs` table + print `[N] pid`
- `jobs` builtin

Out:
- `fg %N` / `bg %N` interactive (would need wait-for-specific-pid + WNOHANG)
- Auto-reap of background zombies — without SIGCHLD-on-exit + WNOHANG, finished bg jobs leak as zombies until reboot or until something else does a `wait()`
- ISIG → terminal-driven Ctrl-C / Ctrl-Z signals (the kbd handler doesn't translate special characters yet)
- Orphaned-pgrp detection (`SIGHUP` / `SIGCONT` to all members when the controlling process exits)
- POSIX restrictions on `setpgid` (must be in same session, can't move across, etc.) — we accept whatever the user passes
- `setsid` rejecting calls from a pgrp leader
- Multiple controlling terminals (we have one tty, fd ignored in `tcsetpgrp`/`tcgetpgrp`)
- `getpriority` / `setpriority`, nice values
- `select` / `poll` on stopped child notifications

## Architecture: three layers, one wakeup

The Unix process-control hierarchy:

```
  Session (sid)   ─── the controlling terminal "owns" one session
       │
       ├── Process group (pgid)  ─── one pipeline = one pgrp
       │     │
       │     ├── Process (pid)
       │     ├── Process
       │     └── Process
       │
       ├── Process group
       │     ├── Process
       │     └── Process
       └── Process group (the "session leader" = the shell, alone)
```

In our model, every `struct task` carries a `pgid` and a `sid`. They default to 0 (kernel tasks, kmain). A user task that calls `setsid()` becomes a session leader (`sid = pgid = pid`) — for our shell, that happens in `main()` before the prompt loop:

```c
setsid();                          /* shell becomes its own session */
tcsetpgrp(0, getpgid(0));          /* shell owns the foreground pgrp */
```

After this:
- Shell's `sid = pid = 5`
- Shell's `pgid = 5`
- TTY's `g_fg_pgid = 5`

Pipelines you launch inherit the shell's session but get their own pgrp. The pipeline's pgid = the pid of the first child. Both the parent and the child call `setpgid` to set up the pgrp; doing it from both sides closes a race window where the child could exec before the parent's setpgid lands.

```c
if (pid == 0) {
    if (i == 0) setpgid(0, 0);              /* leader */
    else        setpgid(0, pgleader);       /* joiner */
    /* ...exec... */
}
pids[i] = pid;
if (i == 0) pgleader = pid;
setpgid(pid, pgleader);                     /* parent-side mirror */
```

Foreground pipelines additionally do `tcsetpgrp(0, pgleader)` before the wait and `tcsetpgrp(0, getpgid(0))` after — handing the tty over and taking it back, so a future ISIG-driven SIGINT would land in the right place.

## TASK_STATE_STOPPED — pause / resume

A new state in the scheduler's skip list. Two parts make it work:

**`schedule()` skips it:**

```c
while (next != g_current
       && (next->state == TASK_STATE_DEAD              ||
           next->state == TASK_STATE_ZOMBIE            ||
           next->state == TASK_STATE_BLOCKED           ||
           next->state == TASK_STATE_BLOCKED_ON_CHILD  ||
           next->state == TASK_STATE_STOPPED)
       && safety--) {
    next = next->next;
}
```

**`signal_check_and_deliver` enters it:**

```c
if (h == SIG_DFL) {
    int act = default_action(sig);
    if (act == ACT_IGN)  return;
    if (act == ACT_CONT) return;
    if (act == ACT_STOP) {
        t->state = TASK_STATE_STOPPED;
        schedule();          /* never returns until we're CONT'd */
        return;              /* fall through to the iret tail */
    }
    /* ACT_TERM: ... */
}
```

The trick is the `schedule()` call inside `signal_check_and_deliver`. We're at the iret-to-ring-3 boundary of a syscall (or IRQ) when we discover the stop signal. We mark the task STOPPED and yield. `schedule()` task-switches to another task; the stopped task's stack pointer is saved with the not-yet-completed iret tail still pending.

Eventually `SIGCONT` arrives. `signal_send` notices the STOPPED state and flips it to READY:

```c
if (sig == SIGCONT) {
    t->sig_pending &= ~((1u << SIGSTOP) | (1u << SIGTSTP));
    if (t->state == TASK_STATE_STOPPED) {
        t->state = TASK_STATE_READY;
    }
}
```

The next `schedule()` round picks the now-READY task. `task_switch` resumes it inside the previous `schedule()` call. `signal_check_and_deliver` returns. The dispatcher tail finishes (`r->eax = ret`). The asm stub does `popa; iret` — the user task resumes at exactly the EIP it was about to return to before the stop, with all registers preserved.

The byte-for-byte resume is what makes job control feel transparent to the stopped program. No syscall returns -EINTR; nothing observed the pause from inside the user code.

## SIGSTOP / SIGCONT semantics

POSIX has subtle rules around stop / cont; we model the important ones:

1. **`SIGCONT` clears any pending `SIGSTOP` / `SIGTSTP`** — sending CONT means "definitely keep running," it shouldn't be possible for an earlier-but-undelivered STOP to fire later.
2. **`SIGSTOP` / `SIGTSTP` clear any pending `SIGCONT`** — symmetrical.
3. **`SIGSTOP` is uncatchable** — `sigaction(SIGSTOP, ...)` returns -1, just like `SIGKILL`.
4. **`SIGTSTP` IS catchable** — programs that want to clean up before stopping (curses programs) install handlers that save state and then `kill(getpid(), SIGSTOP)` to actually stop.
5. **`SIGCONT` default action is "do nothing past the wakeup"** — the wake happens in `signal_send`; the dispatcher-time `ACT_CONT` is a no-op.

```c
case ACT_CONT: return;                  /* wake already happened */

if (sig == SIGCONT) {
    t->sig_pending &= ~((1u << SIGSTOP) | (1u << SIGTSTP));
    if (t->state == TASK_STATE_STOPPED) t->state = TASK_STATE_READY;
}
if (sig == SIGSTOP || sig == SIGTSTP) {
    t->sig_pending &= ~(1u << SIGCONT);
}
```

Compose: send SIGSTOP + SIGCONT in quick succession to a task. The first leaves SIGSTOP pending; the second clears it AND wakes if stopped. Net effect: task keeps running. Right.

## signal_send_pgrp — broadcast

```c
int signal_send_pgrp(uint32_t pgid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;
    if (pgid == 0)              return -1;     /* would hit kernel tasks */

    int delivered = 0;
    for (uint32_t i = 0; i < 16; i++) {
        struct task *t = task_at(i);
        if (!t)                                         continue;
        if (t->pgid != pgid)                            continue;
        if (t->state == TASK_STATE_DEAD ||
            t->state == TASK_STATE_ZOMBIE)              continue;
        if (signal_send(t->id, sig) == 0) delivered++;
    }
    return delivered > 0 ? 0 : -1;
}
```

Walks the task table, signals every live task with matching pgid. `pgid == 0` is rejected — it would otherwise hit kmain (which has pgid 0). POSIX `kill(0, sig)` actually means "send to my own pgrp," which is fine, but we don't model it; users explicitly pass `getpgid(0)`.

The `t11` selftest demonstrates it cleanly: shell forks two children, both joined to a single pgrp via `setpgid(child, pid1)`, then one `killpg(pid1, SIGTERM)` terminates both — kernel logs `[sig] pid=18 terminated by signal 15` and `[sig] pid=19 terminated by signal 15` from each one's `signal_check_and_deliver` default-TERM path.

## The seven syscalls

All thin C wrappers that handle the `pid == 0` "calling task" convention and bounds-check what they can:

| Syscall | What it does |
|---|---|
| `SYS_SETPGID(pid, pgid)` | Set the task's pgid; `pgid == 0` means "use pid" |
| `SYS_GETPGID(pid)` | Read pgid |
| `SYS_SETSID()` | Self only; becomes session + pgrp leader (`sid = pgid = pid`); returns new sid |
| `SYS_GETSID(pid)` | Read sid |
| `SYS_KILLPG(pgid, sig)` | `signal_send_pgrp` |
| `SYS_TCSETPGRP(fd, pgid)` | Set tty's foreground pgrp (fd ignored — single tty) |
| `SYS_TCGETPGRP(fd)` | Read tty's foreground pgrp |

Real POSIX has restrictions: you can only `setpgid` a task into a pgrp in your own session; you can only `setsid` if you're not already a pgrp leader; you need permission to signal other-user tasks. We don't enforce any of those — single-user system, trust the caller.

## Shell side: pgrp-per-pipeline + foreground tty handoff

Before this session a shell pipeline was just "fork each stage, wire pipes, wait for all." After this session the pipeline takes ownership of the tty:

```c
int pgleader = 0;
for (int i = 0; i < n; i++) {
    int pid = sys_fork();
    if (pid == 0) {
        if (i == 0) setpgid(0, 0);              /* leader */
        else        setpgid(0, pgleader);       /* joiner */
        /* ... pipe wiring + exec ... */
    }
    pids[i] = pid;
    if (i == 0) pgleader = pid;
    setpgid(pid, pgleader);                     /* parent mirror */
}

if (pl->bg) {
    /* Background: register in jobs table, print [N] pid, return. */
    /* ... */
    return 0;
}

/* Foreground: tty handoff, wait, take it back. */
tcsetpgrp(0, pgleader);
/* ... wait loop ... */
tcsetpgrp(0, getpgid(0));
```

The double `setpgid` (child + parent) is the canonical idiom. It exists because:

```
fork() returns
   ├── child:  may exec INSTANTLY — before parent's setpgid runs
   └── parent: may not get scheduled for many ticks
```

If the child execs before the parent calls `setpgid(child, pgleader)`, the new program might do something pgrp-sensitive while still in the parent's pgrp. The fix is to make the child call `setpgid` itself — and the parent does it too because the *parent* might race with the child's setpgid (if the parent does `tcsetpgrp(child_pgid)` before the child sets it). Both calls are idempotent, so doing both is safe.

For background pipelines (`cmd args &`), the shell skips the wait and tcsetpgrp dance, registers the job in `g_jobs`, and prints `[N] pgid` — Bash-style. The job table is a fixed-size array of `{in_use, pid, job_id, cmd}`; `jobs` builtin walks it.

## Background jobs and the leak

There's a real limitation: when a background job exits, it becomes a ZOMBIE (per session 14's exit logic) and stays that way. Real Unix shells:

1. Get a `SIGCHLD` from the kernel when any child exits.
2. Have a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop to reap.
3. Mark the corresponding job entry as Done.
4. Print "[N] Done cmd" on the next prompt.

We have neither `SIGCHLD`-on-exit nor `WNOHANG`. So:

- `cmd &` works — fork happens, job table records it.
- `jobs` shows it as Running indefinitely.
- The zombie sits in the task table until reboot or until the user runs `wait` (which would block on the next live child, harvesting one of them along the way — clumsy).

Adding the two primitives to the kernel is straightforward:

- In `task_exit_current`, after marking the task ZOMBIE, send `SIGCHLD` to the parent.
- Add `WNOHANG` flag to `SYS_WAIT`: return 0 immediately if no zombie is ready.

Both are deferred. The deep dive notes them as the obvious next step.

## What the t10 / t11 tests demonstrate

**t10** is the lifecycle:

```
[child 17] tick 1            ← child running
[parent] -> SIGSTOP 17       ← parent stops it
                             ← (no ticks for 80 ms)
[parent] -> SIGCONT 17       ← parent resumes it
[child 17] tick 2            ← child runs again
[parent] -> SIGTERM 17
[sig] pid=17 terminated by signal 15
child reaped: exit=143  (= 128 + 15 = SIGTERM)
```

The 80ms gap with no `[child 17] tick N` lines is the visible proof that STOPPED actually pauses scheduling. The kernel's signal-default `[sig] pid=17 terminated by signal 15` line confirms SIGTERM hit the default ACT_TERM path. Exit code 143 = 128 + 15, the canonical encoding for "killed by SIGTERM."

**t11** is the broadcast:

```
shell sid=5 pgid=5                              ← setsid worked
  [c1 pid=18 pgid=18] tick 1                    ← c1 set itself as pgrp leader
  pgrp 18 has pid1=18 pid2=19
  [c2 pid=19 pgid=18] tick 1                    ← c2 joined pgrp 18
  ...
  -> killpg(18, SIGTERM)
[sig] pid=19 terminated by signal 15            ← both members hit
[sig] pid=18 terminated by signal 15
both reaped: c1 exit=143  c2 exit=143
```

Both children print their own `pid` and `pgid` in their tick lines. Both are in pgrp 18. A single `killpg(18, SIGTERM)` terminates both. The shell's own `sid=5` (= shell's pid) confirms `setsid()` ran at startup.

## Files added / modified

| File | Change |
|---|---|
| `kernel/task.{h,c}` | `pgid`/`sid` fields; `TASK_STATE_STOPPED`; schedule skips it; state-name "STOP"; fork inherits pgid+sid |
| `kernel/signal.{h,c}` | SIGCONT/SIGSTOP/SIGTSTP defines; `default_action` returns ACT_STOP/ACT_CONT; signal_send wakes STOPPED on CONT + clears reciprocal pendings; `ACT_STOP` path enters STOPPED via schedule; SIGSTOP uncatchable; `signal_send_pgrp` |
| `kernel/syscall.{h,c}` | 7 new syscalls (SETPGID/GETPGID/SETSID/GETSID/KILLPG/TCSETPGRP/TCGETPGRP); `find_task_by_pid` helper |
| `kernel/tty.{h,c}` | `g_fg_pgid`, `tty_get_fg_pgrp`, `tty_set_fg_pgrp` |
| `user/libuser.{h,c}` | POSIX-named wrappers; SIGCONT/SIGSTOP/SIGTSTP defines |
| `user/sh.c` | `setsid` + `tcsetpgrp` at startup; pipeline children setpgid (leader / joiner); `&` tokenize + parse + bg path; `jobs` builtin + `g_jobs` table; t10 + t11 selftests |

## Design decisions

**One global `g_fg_pgid` instead of per-tty struct.** We have one tty. Per-tty would be the right shape for a future PTY layer, but adding it now would just push state around without buying anything. The migration is cosmetic — a `tty_get_fg_pgrp(tty *)` taking a tty pointer instead of the global.

**setpgid called from BOTH parent and child.** Documented above — the canonical idiom, idempotent, removes a race that's otherwise invisible until you have a fast-exec'ing program (cat, echo) in a pipeline.

**`pgid == 0` is rejected by `signal_send_pgrp`**. Default for kernel tasks is 0 — a `killpg(0, sig)` would otherwise broadcast to kmain, the reaper, demo tasks, httpd. POSIX uses `kill(0, sig)` as a special sentinel for "my own pgrp"; we don't, but we'd want to eventually.

**`SIGSTOP` is uncatchable.** Same as `SIGKILL`. `sigaction(SIGSTOP, h)` returns -1. `SIGTSTP` IS catchable — programs that want a clean shutdown on Ctrl-Z install a handler that does cleanup + `kill(getpid(), SIGSTOP)` to actually pause.

**`SIGCONT` wakes immediately, doesn't wait for delivery.** The wakeup is a direct state-flip in `signal_send`. The signal stays pending and gets delivered (default = no-op) on next iret. Real Unix is more nuanced — CONT can be caught and the handler runs after the wake. Our model handles handlers correctly via the normal `signal_check_and_deliver` path; the only unique bit is the early state-flip.

**No SIGCHLD-on-exit yet.** Background jobs leak as zombies until reboot. Documented as next-session work alongside `WNOHANG`.

**`tcsetpgrp` and `tcgetpgrp` ignore the fd argument.** We have one tty. The fd parameter is preserved in the API for POSIX shape, ignored in the implementation.

**Fork inherits pgid + sid verbatim.** POSIX-correct. The child can `setpgid(0, 0)` to break out into its own pgrp.

**Exec preserves pgid + sid.** Also POSIX-correct. Pgid is a property of the *process*, not its address space; exec replaces the address space but keeps pgid/sid.

**Foreground pipeline → tcsetpgrp(pgleader); after wait → tcsetpgrp(getpgid(0)).** Standard "pgrp pong" between shell and pipeline. The shell's tcsetpgrp at startup ensures it owns the tty initially; each pipeline borrows it and gives it back.

**`&` is a top-level operator, not allowed mid-pipeline.** `cmd1 & cmd2` would mean two separate commands in real shells (`;` semantics). We enforce `&` only at the very end of the line. A `cmd1 & cmd2` would parse as a single pipeline with `cmd2` after the `&` — which is then in the trailing position and triggers our "must be last" check. Actually we only check that the LAST token is `&`; an earlier `&` would be a parse error. Either way, no support for multi-command lines.

**Job IDs are monotonic from 1.** Never reused within a boot. Mirrors most shells.

## Pitfalls

1. **`schedule()` from inside `signal_check_and_deliver` for ACT_STOP** — works because `r` (the saved frame) lives on the kernel stack we're not freeing, and `task_switch` saves the right ESP. But it relies on the dispatcher tail (`r->eax = ret`) NOT being run before we yield, which is fine because `signal_check_and_deliver` is called *after* `r->eax = ret` already happened. After CONT we resume, `signal_check_and_deliver` returns, we fall out of the dispatcher, asm stub pops + irets. Resume is byte-for-byte clean.
2. **Stopped task in `sys_wait` from the parent's POV** — `has_any_live_or_zombie_child` correctly returns 1 for a STOPPED child (it's neither DEAD nor UNUSED), so wait keeps blocking. But there's no notification when a child stops — the parent has no way to know "my child stopped" without polling getpgid/state. POSIX surfaces this via `WIFSTOPPED` macro on wait's status word; we don't.
3. **`SIGCONT` after `SIGSTOP` in fast succession** can leave neither pending — order matters. Send STOP first, then CONT: STOP queues, then CONT clears it AND wakes (which is fine, task was stopped by STOP, CONT cleared it before delivery, no harm). Send CONT first, then STOP: CONT queues + clears (nothing to clear), STOP queues + clears CONT. Net: STOP pending. Task gets stopped on next iret. ✓
4. **Background zombies leak.** Documented — no SIGCHLD-on-exit, no WNOHANG.
5. **`tcsetpgrp` from a fork'd pipeline child** is not enforced to be in the same session — a child could do `tcsetpgrp(0, 999)` and steal the foreground pgrp from a different session. POSIX requires session check. We don't.
6. **`setsid` doesn't reject session leaders.** A second `setsid` from the shell would no-op (same sid), but a programmatic loop would also be ignored cleanly. POSIX would return -1.
7. **`jobs` table doesn't auto-clean.** Even after a background job dies, its entry stays Running. The user has no way to clear it.
8. **No `wait`-for-specific-pid.** `sys_wait` returns the first available zombie among the caller's children. With multiple background jobs, you can't wait for a specific one. POSIX `waitpid(pid, &status, 0)` would handle it.
9. **The `&` parser only handles `&` at the very end.** `cmd1 & cmd2` is a syntax error (or worse, treated as two-stage pipeline). Real shells parse `&` and `;` as command separators.
10. **No ISIG.** A real Ctrl-C from the keyboard is buffered as `0x03` and read by canonical `sys_read_line`, which echoes it (`^C` in real terminals; `?` in our printable check) but doesn't generate SIGINT. Adding ISIG means the kbd handler scans for `0x03`/`0x1A` and translates to `signal_send_pgrp(g_fg_pgid, SIGINT/SIGTSTP)`.

## What might come next

The natural next session is the missing piece for usable bg jobs: `SIGCHLD`-on-exit + `WNOHANG` flag on wait. With those, the shell's prompt loop scans `g_jobs` cheaply on each prompt, prints `[N] Done` lines, frees zombies. Then `fg %N` and `bg %N` builtins become trivial — `fg`: `tcsetpgrp` + `kill(-pgid, SIGCONT)` + `waitpid(-pgid, ..., 0)`; `bg`: just the SIGCONT.

After that, ISIG in the TTY layer turns Ctrl-C / Ctrl-Z from "characters in the read buffer" into real interrupts. Combined with bg jobs and proper wait, the shell starts to feel like a real one.

Beyond that: orphan-pgrp detection (parent-of-the-pgrp dies → kernel sends SIGHUP+SIGCONT to remaining members), then real session leader / controlling-tty discipline (so a daemon can `setsid` to detach from its tty, the canonical "go background and stay alive past logout" pattern).
