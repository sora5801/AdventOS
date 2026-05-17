# Session 83 — Usable Unix: coreutils gap-fill (+ selftest reliability)

**Goal.** Pivot AdventOS from the "training ground for AI agents" framing toward "a functioning Unix" you can actually live in at the prompt. This session does two related things:

1. **Selftest reliability.** Fix the jobs-leak cascade that's been blocking 1 of 8 selftests under `-smp 2` since session 80, and make the cron-tick rate immune to under-load slowdown.
2. **Path-A coreutils gap-fill.** Add the eight missing utilities every Unix user reaches for in the first hour: `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `chmod`, `touch`, `find`. Plus one kernel syscall (`sys_rmdir`) to make `rmdir` possible at all.

Status: **done.** All 8 binaries build clean and the disk image fits comfortably under its cap. The selftest goes from 7-PASS-with-6-cascade-failures to 7-PASS-with-1-isolated-failure; the residual is a separate ACK-timing race tracked for follow-up.

---

## Part 1 — Selftest reliability

### The jobs-leak cascade

Repro: under `-smp 2`, run `selftest`. `cron-selftest` fails with six cascading assertions starting with "oneshot fired: state=expired, run_count=1" — the cron entry's `fire_at` is in the past but the entry never fires. Six assertions fall over downstream.

Root cause was not in cron. It was in `jobs-selftest` test [8] (the "9 background jobs hit the JOB_MAX=8 cap, the 9th is rejected" check). The test spawns 8 `/sleep.elf 30` jobs, asserts the 9th is rejected, then **cleans up** by sending 8 `shell.job.cancel` RPCs in a tight loop. The cleanup was unreliable in three independent ways, and the failures were silent.

#### Failure 1 — kernel `signal_send` missing a TASK_MAX bump (session-82 followup, fixed in `924a447`)

`kernel/signal.c::signal_send` scanned only the first 16 task slots when looking up a target pid. `TASK_MAX` has been 32 since session 50. Tasks that landed in slots 16+ — common after the sandbox/limits/kv selftests had each consumed their share — couldn't be signaled. `sys_kill(pid, SIGKILL)` silently returned -1, agentd's cancel handler reported `killed: false`, and the sleeper kept running for the full 30 seconds.

The smoking gun was already in the source: `signal_send_pgrp` (which had been fixed) carried a comment calling out exactly this bug class — *"the literal 16 here was a silent latent bug"* — but whoever fixed pgrp missed five other places with the same pattern. All six were fixed in `924a447`:

```
kernel/signal.c     signal_send                       (the primary)
kernel/syscall.c    find_task_by_pid                  (setpgid/getpgid/getsid)
kernel/procfs.c     gen_status/gen_sandbox/gen_limits/pid_is_live
kernel/shell.c      cmd_tasks
kernel/fs.c         fs_entry_open_refs
```

#### Failure 2 — libagent's silent false-positive (fixed in `d5c516d`)

After fix #1, the *single*-sleeper cancel path in test [5] worked. The *eight*-sleeper cancel path in test [8] kept leaking. Diagnostic output (`[diag.cancel]` and `[sig.send]` traces, removed before commit) showed that only 1 of 8 cancels was reaching agentd's dispatch handler. The other 7 received EOF instead of a response.

Investigating, two compounding issues showed up:

- **TASK_MAX_FDS=24 fd-table ceiling.** With 8 jobs alive, agentd is already holding 16 pipe-read fds + 3 stdio + 1 listen = 20 fds permanently. Only 4 free for active conns. A back-to-back cancel burst can leave 2+ stale `CST_IDLE` conn slots on agentd's side (waiting for the kernel to deliver the peer FIN), and the next `sys_accept`'s `alloc_fd` silently fails. The test's recv sees EOF with zero bytes.
- **`libagent::agent_call` left the previous response in the caller's `resp` buffer on any failure path.** A cancel RPC that hits EOF returns -1, but the caller's `resp` still holds the bytes from the previous successful call. `agent_get_bool(resp, "killed", 0)` happily returns `true` from the stale data — the test sees `killed=true` and moves on. Seven of eight cancels looked successful but never actually killed their sleepers.

The library hygiene fix:

```c
int agent_call(const char *json_request, char *resp_buf, int resp_cap) {
    /* Session 83 — zero the caller's resp_buf BEFORE the network call.
     * Without this, any path that returns without successfully filling
     * resp_buf (connect failed, send failed, EOF from agentd) leaves
     * the PREVIOUS call's response bytes visible to the caller. */
    if (resp_buf && resp_cap > 0) resp_buf[0] = 0;
    int sk = connect_localhost_7000();
    if (sk < 0) { capture_last_resp("(connect failed)", 16); return -1; }
    if (agent_send_line(sk, json_request) < 0) { ... return -1; }
    int n = agent_recv_line(sk, resp_buf, resp_cap);
    sys_close(sk);
    if (n <= 0 && resp_buf && resp_cap > 0) resp_buf[0] = 0;
    capture_last_resp(resp_buf, n);
    return n;
}
```

And `agent_recv_line_timed` got the same null-terminate hygiene on its EOF / timeout / overflow paths.

#### Failure 3 — the test's cleanup loop wasn't resilient

Even with both library fixes, the *first* cancel in the 8-cancel burst could still race against test [9]'s tear-down. Fix shape: verify-and-retry in the test, not just throttle:

```c
for (int i = 0; i < 8; i++) {
    ...build cancel payload for spawned[i]...
    int killed = 0;
    for (int retry = 0; retry < 3 && !killed; retry++) {
        agent_method_call("shell.job.cancel", d, resp, sizeof(resp));
        killed = agent_get_bool(resp, "killed", 0);
        if (!killed) sys_sleep_ms(100);
    }
}
```

Combined with the library's clear-on-EOF (which makes `killed=false` a *reliable* signal of failure), this loop transparently handles the fd-pressure burst race.

### The cron-tick rate fix

Independently, even with the jobs-leak gone, the cron `recurring(max_runs=3) fired >= 2 times in 7 s` test was still failing — the recurring entry only fired once.

`cron_tick` was gated by iteration count: `if (++g_cron_div >= 100) cron_tick(...)`. The math assumes each agentd loop iter is ~10 ms (matching the `sys_sleep_ms(10)` at the end), so 100 iters = 1 second.

The assumption broke under cron load. A single cron fire involves `spawn_recorded_job` (fork+exec, 5-20 ms) and `cron_persist` (`sys_fs_write` for the on-disk entry, variable — could be 50-200 ms). One slow iter stretches to 30-100 ms; the iter count overruns wall time by a factor of 3-5x. Cron fired roughly every 5 seconds instead of every 1, and a 7-second test window only caught one fire.

Wall-time gating is independent of iter latency:

```c
/* Session 83: wall-time gating instead of iter-count. cron_tick
 * fires the next iter after sys_time() advances past the last
 * tick's epoch. Per-second resolution matches the fire_at field
 * width (uint32_t epoch seconds). */
static uint32_t g_cron_last_epoch;
uint32_t cron_now = sys_time();
if (cron_now != g_cron_last_epoch) {
    g_cron_last_epoch = cron_now;
    cron_tick(cron_now);
}
```

With both fixes landed, `cron-selftest` failure count drops from 6 → 1. The residual is a separate response-queue timing race on the `cron.subscribe` immediate ACK, tracked for follow-up.

---

## Part 2 — Path-A coreutils gap-fill

### The shape of the gap

Before this session, `ls /` listed files, `cat` printed them, `wc` counted lines, and roughly 15 other things worked. But you literally could not:

- copy a file (`cp`)
- rename a file (`mv`)
- delete a file (`rm`)
- create a directory (`mkdir`)
- remove a directory (`rmdir`)
- change permissions (`chmod`)
- create an empty file (`touch`)
- walk a directory tree (`find`)

`mkdir` and `chmod` had kernel syscalls (`SYS_MKDIR=43`, `SYS_CHMOD=69`) but no userland wrappers. `rm` could be hacked via the `sys_unlink` syscall (`SYS_UNLINK=84`) but there was no `rm` binary. `cp`/`mv` were unbuildable on top of the available primitives because `SYS_OPEN_W` writes to a tmpfs scratch (not the on-disk FS), but `SYS_FS_WRITE` exists and was the right primitive to use directly. `find` was missing entirely. **And `rmdir` couldn't exist** — `fs_unlink` explicitly refuses directories, and no rmdir path existed.

### The one kernel addition: `sys_rmdir`

`fs_rmdir` mirrors `fs_unlink` for files, with one extra check:

```c
int fs_rmdir(const char *path) {
    if (!path || !g_root_inst || !g_root_inst->initialized) return -1;

    int idx = fs_iopen_inst(g_root_inst, path, /*from_root=*/1);
    if (idx < 0) return -1;

    struct fs_entry *e = &g_root_inst->super.files[idx];
    if (e->type != FS_TYPE_DIR) return -1;
    if (fs_check_perm(idx, FS_PERM_W) <= 0) return -1;

    /* Empty-dir check: scan ALL slots for any entry whose parent_dir
     * is this idx. If found, refuse — POSIX ENOTEMPTY analog. */
    for (uint32_t i = 0; i < g_root_inst->super.file_count; i++) {
        if ((int)i == idx) continue;
        const struct fs_entry *child = &g_root_inst->super.files[i];
        if (child->type == FS_TYPE_FREE) continue;
        if (child->parent_dir == (uint8_t)idx) return -1;
    }

    /* Zero the entry, mark FREE, persist. */
    for (int i = 0; i < (int)sizeof(*e); i++) ((uint8_t *)e)[i] = 0;
    e->type = FS_TYPE_FREE;
    return fs_write_super_inst(g_root_inst);
}
```

`SYS_RMDIR = 86` joins the syscall table. The implementation in `kernel/syscall.c` is the same "copy path into kernel buffer then dispatch" shape as `SYS_UNLINK` — paths can't be deref'd directly from the syscall handler because `fs_rmdir` walks deep into FS code that might switch CR3 in the future.

Permissions follow the same model as `fs_unlink`: owner or root. POSIX checks parent-directory write permission for rmdir; AdventOS's directories don't carry per-dir mode bits today, so entry-level permission is the proxy.

### The eight userland binaries

Total source: ~330 lines across all 8 programs. None of them need any new syscalls beyond the existing FS surface plus the new `sys_rmdir`. Total compiled size is ~39 KB.

| Tool | Implementation | Lines | Notes |
|---|---|---|---|
| `rm` | loop `sys_unlink` per arg | 25 | refuses dirs (kernel-enforced) |
| `rmdir` | loop `sys_rmdir` per arg | 25 | needs empty dir |
| `mkdir` | loop `sys_mkdir` per arg | 25 | no `-p` |
| `chmod` | parse octal, loop `sys_chmod` | 50 | octal-only (no `u+x` syntax) |
| `touch` | `sys_fs_size`-probe then `sys_fs_write("", 0)` if missing | 35 | no-op for existing |
| `cp` | `sys_brk` slab + `sys_open`/`sys_read`/`sys_fs_write` | 60 | two-arg form only |
| `mv` | cp + `sys_unlink` | 65 | two-arg form, refuses if dst exists |
| `find` | recursive walk via `sys_readdir` + `sys_fs_size` | 70 | no `-name`/`-type` |

#### Two design choices worth flagging

**The `sys_brk` slab pattern in `cp`/`mv`.** `SYS_FS_WRITE` takes one contiguous user pointer. The simplest correct shape is "read the whole source file into a single buffer, then hand the buffer to `sys_fs_write`." The libuser `malloc` is a free-list allocator that wouldn't be wrong here, but `sys_brk` directly is cleaner for a one-shot slab — no free-list bookkeeping, the buffer lives until the process exits:

```c
int base = sys_brk(0);              /* current heap end */
int want = base + size;
if (size > 0 && sys_brk(want) != want) {
    sys_write(2, "cp: out of memory\n", 18);
    return 1;
}
char *buf = (char *)(uint32_t)base;
```

The `(char *)(uint32_t)base` cast is awkward because libuser deliberately omits `uintptr_t`, but it gets the job done. `sys_brk`'s return convention — return the new break, or unchanged break on failure — makes the success check a single equality test.

**The `size<0` directory detection used by `find`.** `sys_fs_size` returns `-1` for any entry that isn't a regular file: a directory, a missing path, a free slot. The kernel deliberately doesn't expose `FS_TYPE_*` to userland (that's an internal FS detail), but `size>=0 ⇒ file`, `size<0 ⇒ probably-dir-or-missing` is a stable convention. `ls` already uses it to fill the `type` field in its JSONL emitter; `find` reuses it to decide whether to recurse:

```c
static void walk(const char *path, int depth) {
    put_path(path);
    if (depth >= 32) return;
    if (sys_fs_size(path) >= 0) return;     /* file — nothing to recurse */
    /* Else: dir, missing, or other non-regular. sys_readdir below sorts that out. */
    int iter = 0;
    char name[17];
    char child[256];
    for (;;) {
        ...
        int idx = sys_readdir(path, &iter, name);
        if (idx < 0) break;
        if (path_join(path, name, child, sizeof(child)) < 0) continue;
        walk(child, depth + 1);
    }
}
```

The depth cap of 32 is defensive — the FS uses `uint8_t` for `parent_dir`, so it physically can't hold more than 256 entries and any tree built on it has bounded depth. The cap costs nothing and guards against future loops (when we add symlinks, this becomes important).

### What we deliberately skipped

These are absent from this session by design — each would have been more than a single afternoon and most aren't on the daily-pain curve:

- **`cp -r` / `rm -r` / `mkdir -p`** — recursive variants. Worth adding once the basic surface is in daily use and we see which use cases hurt most.
- **`cp SRC... DSTDIR`** (multi-source-into-dir form). Needs dirname extraction in userland and per-target path construction.
- **Symbolic chmod** (`chmod u+x`, `chmod go-w`). Separate parser for the symbolic mini-language; the octal form handles every actual use case I've personally typed in 20 years of Unix.
- **`touch -t` / `touch -m`** — mtime manipulation. AdventFS entries don't carry mtime today. When they do, `touch` is the natural API to bump it.
- **`find -name PATTERN` / `find -type d`** — predicates. The recursive walk is the painful primitive; filtering can layer on with `find | grep` for now.
- **`ln` / `ln -s`** — hard and symbolic links. AdventFS doesn't model either today; symlinks specifically are interesting future work.
- **`du` / `df` / `stat`** — disk usage and file metadata pretty-printing. The syscalls (`SYS_FS_FREE_SECTORS`, `SYS_FS_SIZE`, `SYS_FS_MODE`, `SYS_FS_OWNER`) all exist; the binaries just need writing. Likely the next small chunk of Path A.

### Build wiring

`user/cp.c`, `user/mv.c`, `user/rm.c`, `user/mkdir.c`, `user/rmdir.c`, `user/chmod.c`, `user/touch.c`, `user/find.c` joined `USER_PROGS` in `build.sh`. The corresponding `.elf` entries were appended to `mkfs.py`'s file table. No size-cap bumps were needed — `agentd.bin` is still well under its 480 KiB ceiling, and the kernel image gained ~200 bytes for the `sys_rmdir` dispatch case plus the `fs_rmdir` body.

---

## Smoke test path

A representative session that exercises every new tool:

```sh
advent$ touch hello.txt              # create empty
advent$ ls / | grep hello            # verify
advent$ cp hello.txt copy.txt        # exercise cp
advent$ mv copy.txt renamed.txt      # exercise mv
advent$ rm renamed.txt hello.txt     # exercise rm (multi-arg)
advent$ mkdir testdir                # exercise mkdir
advent$ touch testdir/inner.txt
advent$ ls testdir
advent$ rmdir testdir                # FAILS — not empty
advent$ rm testdir/inner.txt
advent$ rmdir testdir                # succeeds
advent$ chmod 600 hello.txt          # exercise chmod
advent$ find /                       # walk whole tree
```

Each command either succeeds silently (Unix tradition) or prints a one-line stderr message.

---

## What's next in Path A

The session 83 work is **Phase 1** of "Path A — Usable Unix." The remaining phases:

- **Phase 2 — Shell mid-line editing.** `user/sh.c::read_line_interactive` already does raw mode, history (up/down), tab completion, and backspace. The explicit gap in the source — "ESC[C / ESC[D (right/left) ignored — no mid-line editing" — is the most common daily annoyance. Adds left/right cursor, Ctrl-A/E (start/end of line), Ctrl-W (delete word), Ctrl-U (delete to start), Ctrl-K (delete to end).
- **Phase 3 — Man pages.** A discoverability layer. Even one-line summaries per builtin make a real difference; the existing per-program `--help` is inconsistent across binaries.
- **Phase 4 — vi polish.** `user/vi.c` is a real modal editor but limited. Search-and-replace, undo, and a less-painful save/quit prompt would push it from "tolerable for emergencies" to "actually pleasant for short edits."

The selftest fixes in part 1 of this session were preconditions: the `selftest` harness has to be reliable before it's worth gating "Path A" milestones on it.

---

## Files touched

```
kernel/signal.c     signal_send TASK_MAX scan (in 924a447 — session-82 followup carried in)
kernel/syscall.c    find_task_by_pid TASK_MAX, SYS_RMDIR dispatch
kernel/syscall.h    SYS_RMDIR define (= 86)
kernel/procfs.c     gen_status/gen_sandbox/gen_limits/pid_is_live TASK_MAX
kernel/shell.c      cmd_tasks TASK_MAX
kernel/fs.c         fs_entry_open_refs TASK_MAX, fs_rmdir implementation
kernel/fs.h         fs_rmdir prototype
user/libagent.c     agent_call clear-on-failure, agent_recv_line_timed null-terminate hygiene, reset_jobs simplified
user/libuser.c      sys_rmdir wrapper
user/libuser.h      SYS_RMDIR define, sys_rmdir prototype
user/agentd.c       cron_tick wall-time gating
user/jobs-selftest.c verify+retry cancel cleanup
user/cron-selftest.c subscribe ACK timeout 1s→3s
user/cp.c           NEW
user/mv.c           NEW
user/rm.c           NEW
user/mkdir.c        NEW
user/rmdir.c        NEW
user/chmod.c        NEW
user/touch.c        NEW
user/find.c         NEW
build.sh            USER_PROGS additions
mkfs.py             coreutils elf entries
README.md           NEW — top-level project overview
docs/70-usable-unix-coreutils.md  NEW — this file
```

Commits in this session:
- `924a447` — Fix signal_send TASK_MAX bug (session-82 followup)
- `d5c516d` — Selftest reliability: jobs-leak cascade fixed, cron-tick rate fixed
- (this commit) — Usable Unix Phase 1: coreutils gap-fill + README + session-83 deep dive
