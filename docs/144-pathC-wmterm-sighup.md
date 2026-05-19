# Session 158 — SIGHUP on PTY master close

Path C phase 51.  Follow-up to session 157.

Session 157's deep-dive ended with a footnote: "wmterm exiting via
the close X does *not* yet send SIGHUP to the child sh.elf.  The
shell stays alive, sitting on its (now master-less) PTY slave,
until it tries to write to fd 1 and gets `-1` from `pty_slave_write`'s
`master_refs == 0` check."  Worse than "stays alive" — it
*spinned*:

```c
/* user/sh.c, read_line_interactive (before this session) */
for (;;) {
    char c;
    int  n = sys_read(0, &c, 1);
    if (n <= 0) continue;          /* burns CPU on EOF */
    ...
}
```

With the master gone, every iteration of that loop did one
`sys_read` that returned 0 immediately, hit the `continue`, and
went round again.  The shell process was using a scheduling slot
to do nothing.  Session 158 closes that out.

## Two layers, one outcome

### 1. Kernel: deliver SIGHUP when carrier drops

POSIX TTYs send SIGHUP to the controlling-terminal's session
leader when the master side disconnects ("modem carrier lost"
in the real-hardware lineage that this name comes from).
AdventOS's PTY already tracks `fg_pgrp` for the Ctrl-C path
that converts incoming 0x03 bytes into SIGINT.  We reuse that
state.

```c
/* kernel/pty.c */
void pty_close_master(int idx) {
    if (!valid(idx)) return;
    struct pty *p = &g_ptys[idx];
    if (p->master_refs > 0) p->master_refs--;
    if (p->master_refs == 0 && p->fg_pgrp > 0) {
        signal_send_pgrp((uint32_t)p->fg_pgrp, SIGHUP);
    }
    try_free(p);
}
```

`fg_pgrp` is set by sh.elf at startup via `tcsetpgrp(0, getpgid(0))`,
so by the time wmterm closes, it points at the inner sh's pgid.
SIGHUP's default action is `ACT_TERM` (see `kernel/signal.c`'s
`default_action`), so the shell — which doesn't install a SIGHUP
handler — dies cleanly.

The `master_refs == 0` guard matters: wmterm's fork+exec dance
goes through several master close points (fork bumps refs, child
closes master to drop to N-1, etc.).  Only the *last* close
sends SIGHUP.

### 2. sh.elf: distinguish EOF from interrupted read

Even with the kernel delivering SIGHUP, sh.elf should still
behave reasonably when *some other* program ends up holding a
PTY slave whose master closed (a non-fg_pgrp shell, a piped sh,
a future GUI app).  The right move is to treat `read() == 0`
as EOF and exit, the way every Unix shell does:

```c
/* user/sh.c, read_line_interactive */
if (n == 0) {
    tty_set_mode(prev_mode);
    return -1;              /* EOF sentinel — was 0-as-empty-line */
}
if (n < 0) continue;        /* signal interrupt / non-block — retry */
```

The outer loop in `main()` then exits the process when
`read_line_interactive` returns -1:

```c
int n = read_line_interactive(line, sizeof(line));
if (n < 0) { puts("\n"); sys_exit(0); }
if (n == 0) continue;       /* empty Enter — redraw prompt */
```

The split between `n == 0` (EOF) and `n < 0` (non-fatal) is
the bit that didn't exist before.  Before, both fell into the
`continue` branch and spun.

## Why both layers

The kernel SIGHUP is the proactive cleanup — kills any
foreground command sh might be running, not just sh itself.
If the user typed `sleep 30` and then closed wmterm,
SIGHUP-to-fg_pgrp brings down `sleep` (cat / vi / whatever
else) too.

The sh-side EOF handling is the fallback.  If a future PTY
client doesn't set fg_pgrp (or sets it to a different group
than sh), SIGHUP-to-fg_pgrp won't reach sh — but sh will still
see EOF on its blocked read and exit.

Belt and suspenders.

## Smoke

`smoke_wmterm_sighup.py` runs end-to-end and asserts on four
serial-log markers:

| Marker                                | Proves                              |
|---------------------------------------|-------------------------------------|
| `wmterm: rd n=88 first=0xa`           | inner sh.elf produced banner output |
| `wmterm: done`                        | wmterm processed CLOSE and exited   |
| `[sig] pid=N terminated by signal 1`  | kernel delivered SIGHUP; sh died    |
| `s158_alive` after `echo s158_alive`  | outer shell survived the whole dance|

The last one is the regression guard: SIGHUP is targeted at the
*inner* sh's pgid (the one running on the PTY slave), not the
outer kernel-console sh that launched wmterm.  Getting that
targeting wrong would kill the wrong shell.  The smoke
explicitly types a command after wmterm exits and checks for the
echo to confirm the outer shell is still interactive.

3/3 fresh-QEMU runs pass.

## What changed, exhaustively

- `kernel/pty.c` — `pty_close_master` delivers SIGHUP on last
  close.
- `user/sh.c` — `read_line_interactive` returns -1 on EOF, and
  `main()` exits on -1.  `n < 0` still retries (interrupted
  read, no payload).
- `smoke_wmterm_sighup.py` — new trace-driven smoke.

## What this *doesn't* fix

- **SIGHUP on slave close**: if the slave side goes away first
  (sh.elf is killed externally, the child of wmterm crashes),
  wmterm gets `master_refs == 1 → 0` only when *it* closes the
  master.  Until then, reads return 0 and the existing logic
  in `pty_master_read`'s while-loop (`slave_refs > 0`) handles
  the empty case.  wmterm's main loop already tolerates n == 0
  via the non-block path (session 157), so no spinning.

- **Multi-session PTYs**: we still don't track session leaders.
  If someone wires up `setsid()` to actually allocate session
  IDs and connects them to PTYs, the SIGHUP target should be
  "the session leader of the controlling terminal" instead of
  "the foreground pgrp".  Today the two are the same in
  practice (`tcsetpgrp(0, getpgid(0))` on shell startup), so
  no observable difference.
