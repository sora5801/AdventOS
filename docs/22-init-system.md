# Session 22 — A real init system + orphan reparenting

**Goal:** Get the kernel out of the boot-policy business. Up through session 21, `kmain` ended with a hardcoded `LAUNCH("httpd.elf"); LAUNCH("sh.elf", "selftest");` — that's the kernel deciding which user services run. A real OS has the kernel spawn exactly one userspace process (init / PID 1), and that process reads a config file and forks the actual services.

This session ships:
- `user/init.c` — a small init that reads `/inittab`, fork+execs each line, then loops in a wait+respawn loop
- `fs/inittab` — the config file
- Kernel reparents orphans to init in `task_exit_current`
- `kmain` is one `LAUNCH("init.elf")` plus a `task_set_init_pid(_t->id)`
- `[t13]` selftest demonstrating orphan adoption end-to-end

End state — the boot output now reads:

```
[boot] launched init.elf as pid 4
init: pid=4, reading /inittab
init: 2 service(s) in inittab
init: started 'httpd.elf' as pid 5 (once)
init: started 'sh.elf' as pid 6 (once)
init: entering reap+respawn loop
... selftest runs ...
[t13] orphan adoption by init
    [child pid=21] exiting (grandchild still alive)
  shell reaped child pid=21 code=7
  grandchild is now an orphan; init should reap it shortly
    [grandchild pid=22] exiting; should be adopted
init: reaped orphan pid=22 code=33
```

That last `init: reaped orphan pid=22 code=33` is the visible proof of reparenting — the grandchild's parent (the child) exited 80 ms before the grandchild did, the kernel rewrote `parent_id = init_pid`, and init's regular `sys_wait` loop picked it up and printed the line. `httpd.elf` keeps serving curl on :80 throughout (`status=200 bytes=317`).

## What's in scope

In:
- `user/init.c` — inittab parser, spawn, wait+respawn loop
- `fs/inittab` — line-oriented config: `<mode> <program> [args...]`, modes are `once` and `respawn`
- `task_set_init_pid` / `task_get_init_pid` in task.c
- `g_init_pid` global; `task_exit_current` reparents the dying task's surviving children to it
- The reparent path also wakes init if it's currently `BLOCKED_ON_CHILD` and the reparented child is already a zombie
- kmain spawns only `init.elf` (no more LAUNCH macros for httpd / sh)
- `[t13]` selftest in sh.c: fork → fork → child exits leaving grandchild orphaned → init reaps

Out:
- Runlevels (`telinit 3`, etc.) — single implicit "boot" runlevel
- SysV-style scripts in `/etc/init.d` with start/stop/restart actions
- systemd-style unit files with declarative dependencies
- `getppid()` syscall (we'd need it for a fully visible "I'm now child of init" check from userspace; we infer it by watching init's stdout)
- Init shutdown handling (poweroff / reboot via signals)
- Process accounting / wall-time tracking
- `setuid` services — there's no user/permission concept yet
- Init-as-PID-1 enforcement — init is whatever pid the kernel happens to give it (4 in our build), since reaper + 2 demo tasks come first
- A real `/etc/inittab` directory; we use a flat-fs file named `inittab`
- Reading services from multiple files (`/etc/init.d/*` glob)
- Conditional spawn (skip if some sentinel file exists, etc.)
- Service dependencies / ordering — services start in inittab order, no waiting

## Architecture: who spawns whom

Before this session:

```
kmain
  |- LAUNCH("httpd.elf", "httpd")
  |- LAUNCH("sh.elf", "sh", "selftest")
  └─ for(;;) hlt
```

After:

```
kmain
  |- LAUNCH("init.elf", "init")
  |  └─ task_set_init_pid(init->id)
  └─ for(;;) hlt

init (pid 4)
  |- read /inittab
  |- fork+exec httpd.elf  ──> pid 5
  |- fork+exec sh.elf selftest  ──> pid 6
  └─ wait loop
        ├─ "init: 'sh.elf' (pid=6) exited code=N"
        ├─ "init: reaped orphan pid=K code=M"  ← reparented grandchildren
        └─ if respawn: fork+exec again
```

`kmain` no longer knows what services exist. Adding a new service is one line in `/inittab` and a rebuild of the FS image — no kernel rebuild needed.

## inittab

Line-oriented config, one service per line:

```
# AdventOS inittab — services started by init at boot.
#
# Format:  <mode> <program> [args...]
#   once    : run once, don't restart on exit
#   respawn : restart whenever this exits

once httpd.elf
once sh.elf selftest
```

`#`-prefixed lines and blank lines are ignored. Each non-blank line:
1. First token = `once` or `respawn`
2. Second token = program name (looked up via `sys_open` → falls through to disk fs / tmpfs)
3. Remaining tokens = argv (passed to `sys_exec`)

The parser is ~30 lines:

```c
static int parse_inittab(char *text) {
    int idx = 0;
    char *p = text;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = 0; p++; }

        while (*line == ' ' || *line == '\t') line++;
        if (*line == 0 || *line == '#') continue;

        struct service *s = &g_services[idx];
        /* copy into s->raw, tokenize in place, mode/argv split */
        ...
        idx++;
    }
    return idx;
}
```

Tokens are NUL-separated by walking the line and replacing whitespace with `\0`. The `argv` array is filled with pointers into `s->raw` and NUL-terminated for `sys_exec`'s expected shape.

`MAX_SERVICES = 16`, `MAX_ARGS = 8`. Plenty for our scale.

## init's main loop

```c
for (;;) {
    int code = 0;
    int pid = sys_wait(&code);
    if (pid < 0) {
        /* No children to wait on — sleep and retry. Allows init to
         * survive a moment when the only orphan is mid-flight. */
        sys_sleep_ms(200);
        continue;
    }

    int idx = find_service_by_pid(pid);
    if (idx < 0) {
        /* Adopted orphan. Init wasn't its original parent — the
         * kernel reparented it when its real parent exited. */
        printf("init: reaped orphan pid=%d code=%d\n", pid, code);
        continue;
    }

    struct service *s = &g_services[idx];
    printf("init: '%s' (pid=%d) exited code=%d\n",
           s->argv[0], pid, code);
    if (s->respawn) {
        spawn_service(s);
        printf("init: respawned '%s' as pid=%d\n", s->argv[0], s->pid);
    } else {
        s->pid = -1;
    }
}
```

Three cases on each `sys_wait` return:

1. **No children** — `pid < 0`. Sleep briefly and retry. This shouldn't happen in normal operation (we always have at least the inittab services) but it's the right thing to do if everything dies.
2. **Known service** — pid matches one in `g_services[]`. Log the exit; if `respawn`, fork+exec a fresh one.
3. **Unknown pid** — adopted orphan. Reap it, log the line.

The "unknown pid" case is the entire reason orphan reparenting matters.

## task_exit_current — the kernel's reparenting

The reparent has to happen at the very moment a task dies, before any of its children look up their parent and find it gone:

```c
void task_exit_current(int exit_code) {
    struct task *t = g_current;
    t->exit_code = exit_code;
    if (t->is_user) close_all_fds(t);

    /* Reparent any of our surviving children to init. */
    if (g_init_pid != 0 && t->id != g_init_pid) {
        for (int i = 0; i < TASK_MAX; i++) {
            if (g_tasks[i].state == TASK_STATE_UNUSED) continue;
            if (g_tasks[i].parent_id == t->id) {
                g_tasks[i].parent_id = g_init_pid;

                /* Edge case: child already became a zombie BEFORE
                 * its parent died. The original parent is gone now
                 * with no chance to reap. Wake init so it picks
                 * up the new orphan immediately. */
                if (g_tasks[i].state == TASK_STATE_ZOMBIE) {
                    for (int j = 0; j < TASK_MAX; j++) {
                        if (g_tasks[j].id == g_init_pid &&
                            g_tasks[j].state == TASK_STATE_BLOCKED_ON_CHILD) {
                            g_tasks[j].state = TASK_STATE_READY;
                        }
                    }
                }
            }
        }
    }

    /* ... existing parent-wake + zombie/dead transition ... */
}
```

Two clauses:

1. **`g_init_pid != 0 && t->id != g_init_pid`** — only reparent if init exists, and don't reparent if WE are init (init dying is its own emergency we don't model — orphans of init would have nowhere to go, and the kernel reaper would clean them up the old way).

2. **The pre-zombie-aware wake** — without it, the orphan adoption hangs if the timing is "child becomes zombie, then its parent dies, then nothing happens." The grandchild's `task_exit_current` already ran; it set state=ZOMBIE and tried to wake the (still-alive then) parent. Now the parent dies; the grandchild's parent_id is rewritten to init, but init is sitting in `BLOCKED_ON_CHILD` waiting for ITS children — it has no idea a new zombie just appeared. Without the explicit wake, the grandchild sits in zombie state forever.

The wake is "scan tasks for the one whose id matches g_init_pid, if it's `BLOCKED_ON_CHILD` flip it to `READY`." Once init runs, its `task_wait_current` loop calls `reap_one_zombie_of(init)` which sees the freshly-reparented zombie and harvests it.

## What [t13] verifies

```
[t13] orphan adoption by init
    [child pid=21] exiting (grandchild still alive)
  shell reaped child pid=21 code=7
  grandchild is now an orphan; init should reap it shortly
    [grandchild pid=22] exiting; should be adopted
init: reaped orphan pid=22 code=33
```

Reading top to bottom:

- Shell calls `sys_fork` → child pid 21.
- Child calls `sys_fork` → grandchild pid 22.
- Child exits 7. Shell reaps it.
- Grandchild is still asleep (in `sys_sleep_ms(80)`).
- The kernel `task_exit_current(7)` for pid 21 ran the reparenting loop: pid 22's `parent_id` got rewritten from 21 to init's pid (4).
- 80 ms later the grandchild wakes, prints, exits 33.
- Now `task_exit_current(33)` for pid 22 looks up its parent — finds init (pid 4) — sets it to ZOMBIE.
- Init was sitting in `sys_wait` (BLOCKED_ON_CHILD). The exit path wakes it.
- Init's wait loop runs. `reap_one_zombie_of(init)` finds pid 22 — its parent_id is init's pid, and it's a ZOMBIE.
- Init harvests + prints `init: reaped orphan pid=22 code=33`.

That's the whole adoption + reaping cycle, end-to-end.

## Files added / modified

| File | Change |
|---|---|
| `user/init.c` | New. ~200 lines: parser + spawn + wait+respawn loop |
| `fs/inittab` | New. 2 active lines (httpd + sh) plus comments |
| `kernel/task.{h,c}` | `g_init_pid`, `task_set_init_pid`, reparenting in `task_exit_current` |
| `kernel/kernel.c` | LAUNCH macro retired; explicit init.elf spawn + `task_set_init_pid` |
| `user/sh.c` | `[t13]` selftest |
| `build.sh` | `init` added to USER_PROGS |
| `mkfs.py` | `init.elf` and `inittab` added to USER_PROGRAMS / DATA_FILES |

## Design decisions

**Init reads a config file, doesn't have services compiled in.** This is the entire point of the milestone. Adding `respawn ntpd.elf` to inittab and rebuilding the FS image is the workflow; rebuilding the kernel isn't.

**Two modes only: `once` and `respawn`.** Real init systems have many more (manual / wait / boot / bootwait / off / sysinit / ...). For the demo, "run-and-forget" and "keep-it-alive" cover the cases. Adding more modes is per-mode pattern matching in the parser plus a state-machine extension in the wait loop.

**Inittab is a flat file in the disk fs.** `sys_open` looks up the disk FS first, then tmpfs (session 15). So the file lookup just works. No directory abstraction.

**Init runs with whatever pid the kernel gives it.** In our build, kmain spawns init.elf after the reaper + 2 demo tasks have used pids 1, 2, 3 — init is pid 4. In real Unix init is literally PID 1. We don't enforce that; we just track "the init pid" via `g_init_pid`. The reparenting target is by pid value, not slot.

**Reparenting touches `parent_id` only.** No "child list" data structure to update. The `parent_id` field is the source of truth; everything that asks "who's my parent?" or "who has me as a child?" walks `g_tasks[]` and matches by pid.

**The kernel's reparent loop has the pre-zombie-aware wake.** Documented above. Without it, the demo hangs in the t13 case where the grandchild is faster than the wake-up.

**Init-die-equals-system-death isn't enforced.** A real Unix kernel panics if PID 1 dies. We don't check; if init exits, the system continues with no reaper of orphans. The kernel reaper would still free orphan-DEAD tasks on its own schedule, so things wouldn't crash — just no `init: reaped orphan` lines.

**Adopted orphans are silently OK to wait for.** Init's `sys_wait` doesn't distinguish "this is one of my known services" from "this is an adopted orphan I never spawned." Both look like "a child of mine just exited." `find_service_by_pid` returns -1 for adopted orphans → init prints the diagnostic line and moves on.

**fork-vs-exec race in `spawn_service`** — fixed by the canonical Unix idiom of having the child exit 127 if `sys_exec` returns. The child doesn't try to keep running; init's wait loop handles it.

**Init never exits.** No clean shutdown path. A `SIGTERM` to init would default-terminate it (TERM is in the no-handler default-action list); since signals to init aren't useful in our model, we don't install handlers.

**`/inittab` (no leading slash in our flat fs).** `sys_open("inittab")` works because there are no directories. A real OS would have `/etc/inittab`.

## Pitfalls

1. **Init dying takes the system down silently.** The kernel doesn't panic; orphans just stop getting reaped via init. The reaper still cleans DEAD slots, so memory doesn't leak — but anything that relies on init (future signal-driven respawn, etc.) goes silently broken.
2. **The reparenting loop walks `g_tasks[]` in slot order.** If two of a dying task's children are in slots, both get reparented — but the wake-init bit only fires once per zombie (the loop continues after wake). Should be fine; init's wait drains all zombies in a row.
3. **`task_set_init_pid(0)` would disable reparenting.** No safeguard; if you accidentally clear it after init is up, future orphans go to DEAD instead of init.
4. **Inittab parsing is unforgiving** — a malformed line ("respawn" without a program, an unknown mode word) prints a diagnostic but doesn't crash. The whole table parses successfully; bad lines just don't yield services.
5. **Init's wait loop has a 200ms sleep on `pid < 0`.** Means an injection of orphans during a "no children" window will be reaped 200 ms later. Real init wouldn't have this window (it has children at all times). For us it's a no-op since inittab has 2 entries.
6. **Respawn doesn't rate-limit.** A service that crashes on startup would respawn forever in a tight loop, eating CPU. POSIX init systems implement "respawn too fast → halt for N seconds" debounce. We don't.
7. **No service dependencies.** httpd starts before sh whether or not the network is up (it is, by the time init runs — DHCP completed in kmain). A service that needs an earlier service to be ready would just race.
8. **`sys_wait` waits for ANY child.** Init has no way to wait for a specific one. With multiple respawn services, init's loop is generic — it figures out which service a returning pid belongs to and acts accordingly. Without a per-service `waitpid`, we couldn't implement priorities or kill-old-restart-new.
9. **The kernel's reparent loop runs UNDER cli (since `task_exit_current` is called from the dispatcher tail, IF=1 but no explicit lock).** Race with anything that mutates `g_tasks[i].parent_id` from another context would be bad — there is no such other context today.
10. **`g_init_pid` is set after init's first task_create_user call.** There's a tiny window between init being scheduled and `task_set_init_pid` being called where init's pid isn't yet "registered" — if init forks+execs in that window, those children would have parent_id=init.id but the reparenting code wouldn't know to use it for OTHER orphans. Actually init's children's parent_id is just init's pid (whatever it is), so this works. No real race.

## What might come next

`SIGCHLD`-on-exit + `WNOHANG` on `sys_wait` so a service's exit can wake init via a signal handler instead of init being permanently parked in `sys_wait`. After that, real `getppid()` so user programs can see they were reparented. Then a userspace `service` builtin in the shell that can ask init to start/stop/restart (via a Unix socket or shared-file rendezvous; we don't have the IPC for it yet).

Then runlevels (multi-user vs single-user), service dependencies (`After=network`-style), and eventually the systemd-style declarative model. None of those make the OS more *capable* in a kernel sense — they're all userspace policy on top of the same fork+exec+wait we already have.
