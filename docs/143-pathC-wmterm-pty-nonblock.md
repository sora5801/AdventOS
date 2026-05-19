# Session 157 — wmterm input + close, fixed at last

Path C phase 50.  The wmterm bug deferred since session 145 (eight
sessions ago — see docs/120-pathC-wmterm.md for the original
build-out) finally goes away.

## The symptom

`wmterm` opened fine, the shell prompt rendered correctly, and the
window decorations were drawn by wmd as expected.  But the moment
you tried to interact with it — type a key, click the close X,
even click the body for focus — *nothing happened*.  The window
sat there as if frozen, and the only way to get rid of it was to
let it time out after 120 seconds.

Once you knew the symptom you could see it on the screen: even
the title bar didn't change colour when you clicked.  No focus,
no keystrokes, no close.

## The root cause

wmterm's main loop has the shape every WM client uses:

```c
for (...) {
    while (wm_poll_event(&win, &ev)) { ... }   /* (a) */
    int n = sys_read(master, buf, sizeof(buf)); /* (b) */
    if (n > 0) for (...) vt_feed(...);
    /* paint */
    sys_sleep_ms(33);
}
```

Step (a) drains FOCUS/KEY/CLOSE/etc. from wmd.  Step (b) drains
whatever the shell wrote since the last frame.  Step (b) is
supposed to be **non-blocking** — wmterm marks the master fd
with `sys_fd_nb(master, 1)` right after `sys_openpty()`:

```c
sys_close(slave);
sys_fd_nb(master, 1);
```

That's the *intent*.  The bug was that `sys_fd_nb` set the
`FD_FL_NONBLOCK` flag in the per-fd state, but the kernel's
read-side dispatcher silently ignored that flag for PTY fds.
Compare these two cases in `kernel/syscall.c` (before this
session):

```c
case FD_PIPE_R:
    if (e->flags & FD_FL_NONBLOCK) {            /* honored */
        int av = pipe_read_avail(e->obj_idx);
        if (av != 1) { ret = -1; break; }
    }
    ret = pipe_read(e->obj_idx, buf, n);
    break;

case FD_PTY_M:                                  /* ignored! */
    ret = pty_master_read(e->obj_idx, buf, n);  /* blocks */
    break;
```

So when wmterm called `sys_read(master, ...)` at step (b), the
kernel dispatched straight into `pty_master_read`, which on an
empty s→m ring sat in a `task_yield()` loop:

```c
while (p->s_to_m_head == p->s_to_m_tail && p->slave_refs > 0) {
    if (signal_interrupted()) return -1;
    task_yield();
}
```

The shell was idle (it just printed `advent$ ` and was now
sitting in its own blocking `sys_read(0, ...)` waiting for the
user).  No bytes flowing in the s→m direction.  wmterm's
`pty_master_read` yielded forever.

That meant step (a) — `wm_poll_event` — ran exactly *once* per
frame, right after the initial banner came through.  Once
wmterm was stuck in the read, the event queue filled up with
FOCUS / MOUSE_MOVE / KEY / CLOSE events that wmd kept pushing,
and wmterm never popped any of them.  From the outside, the
window looked dead.

## The fix

Three small kernel changes, plus a smoke that proves the
round-trip end-to-end.

### 1. Two peek helpers in `kernel/pty.c`

```c
int pty_master_read_avail(int idx) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    return (p->s_to_m_head != p->s_to_m_tail) ? 1 : 0;
}

int pty_slave_read_avail(int idx) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    return (p->m_to_s_head != p->m_to_s_tail) ? 1 : 0;
}
```

Both are tiny — head ≠ tail means at least one byte is
available, same lock-free SPSC invariant the read/write helpers
already rely on.  Declared in `kernel/pty.h` next to the rest of
the PTY API.

### 2. Honor `FD_FL_NONBLOCK` in the syscall dispatcher

`kernel/syscall.c`, in the `sys_read` switch:

```c
case FD_PTY_M:
    if (e->flags & FD_FL_NONBLOCK) {
        int av = pty_master_read_avail(e->obj_idx);
        if (av != 1) { ret = 0; break; }
    }
    ret = pty_master_read(e->obj_idx, buf, n);
    break;
case FD_PTY_S:
    if (e->flags & FD_FL_NONBLOCK) {
        int av = pty_slave_read_avail(e->obj_idx);
        if (av != 1) { ret = 0; break; }
    }
    ret = pty_slave_read(e->obj_idx, buf, n);
    break;
```

`ret = 0` rather than `-1`: wmterm's existing `if (n > 0)`
guard treats both the same, but `0` matches the EOF/"no data
this frame" semantics that the rest of the file already uses
for FD_CDC_ACM in non-block mode.  Pipes return `-1` historically
(EAGAIN flavor); we don't touch that.

### 3. A small TOCTOU note

There's a tiny race between `pty_master_read_avail` returning
1 and `pty_master_read` actually reading — but wmterm is the
sole owner of the master fd in this design, so no other task
can race the drain.  If we ever multiplex master fds across
threads we'll need to fold the peek into the read itself
behind the same internal SPSC bookkeeping.

## Why the close-X looked broken too

Same bug, same fix.  CLOSE arrives over the same WM event
queue as KEY and FOCUS, and `wm_poll_event` is the only path
to it.  Step (b) blocking → step (a) never runs again →
CLOSE event piles up unread, window stays put forever.

## Diagnostic prints, gated

`wmterm` now takes an optional `-v` flag (or you can hand-set
`g_verbose`) that prints one line per FOCUS / UNFOCUS / KEY /
read.  Default is quiet so a normal interactive session
doesn't spam its launching console.  The smoke launches
`wmterm -v 50` and uses these prints as the verification
signal — pixel-based verification of typing was flaky on
QEMU's USB-tablet (which is the actual subject of the next
gnarl, not session 157).

## Smoke

`smoke_wmterm_fix.py` is trace-driven, not pixel-driven.  It
watches `wmterm:`-prefixed lines on the serial console for
five markers, retrying clicks up to ten times each (QEMU's
usb-tablet drops absolute reports occasionally and the cursor
ends up at the previous position):

| Marker                          | Proves                                      |
|---------------------------------|---------------------------------------------|
| `wmterm: rd n=88`               | shell→wmterm path: sys_read returns data    |
| `wmterm: FOCUS`                 | wmd→wmterm event delivery is alive          |
| `wmterm: KEY 0x61 wr=1`         | KEY event reached wmterm, write to PTY OK   |
| `wmterm: rd n=1 first=0x61`     | full PTY round-trip: sh.elf echoed 'a' back |
| `wmterm: done`                  | CLOSE event handled, wmterm exits cleanly   |

The background drainer thread is non-negotiable.  Without it the
host TCP serial buffer fills, QEMU stops draining the UART, the
kernel's `serial_putc` busy-waits in `while (!tx_empty()) {}` —
and wmterm hangs in its own diagnostic printf.  That'd look
identical to the original bug from the outside; we'd be chasing
a phantom.

Result over 4 consecutive fresh-QEMU runs: 4/4 pass.

```
[OK] shell banner round-trip (rd n=88)
[OK] wmd -> wmterm FOCUS event delivery
[OK] KEY 'a' routed to wmterm
[OK] PTY echo round-trip 'a' -> wmterm
[OK] CLOSE event -> wmterm exits cleanly
```

## What changed, exhaustively

- `kernel/pty.h` — added `pty_master_read_avail` and
  `pty_slave_read_avail` declarations.
- `kernel/pty.c` — added the two peek functions.
- `kernel/syscall.c` — `FD_PTY_M` and `FD_PTY_S` cases in
  `sys_read` honor `FD_FL_NONBLOCK`.
- `user/wmterm.c` — added `-v` flag and the gated diagnostic
  prints.  The main loop itself is otherwise unchanged — the
  only reason the loop didn't work before was the kernel side.
- `fs/man/wmterm` — documented `-v` and added a HISTORY note
  pointing at this fix.
- `smoke_wmterm_fix.py` — new, trace-driven, 4/4 reliable.

## What this *doesn't* fix

The USB-tablet flake (sometimes the very first abs report
after a quiet idle is dropped by QEMU) is real and visible in
the smoke as needing to retry clicks.  Not a kernel bug, just
QEMU virtual hardware behaviour; documented in
docs/127-pathC-usbtablet.md.

SIGHUP-on-close — wmterm exiting via the close X does *not*
yet send SIGHUP to the child sh.elf.  The shell stays alive,
sitting on its (now master-less) PTY slave, until it tries to
write to fd 1 and gets `-1` from `pty_slave_write`'s
`master_refs == 0` check.  Wiring SIGHUP cleanly is its own
session — see `man wmterm` HISTORY.
