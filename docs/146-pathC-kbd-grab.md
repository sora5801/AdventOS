# Session 160 — keyboard grab (the real reason typing was eaten)

Path C phase 53.  Bug-fix follow-up to sessions 157-159.

## The user complaint

> "Typing into wmterm still doesn't show the characters."

After session 157 (PTY non-block), 158 (SIGHUP-on-close), and
159 (scrollback), every smoke test passed.  The user ran wmterm
interactively and reported it *still* didn't work.

The smoke was lying — not about the fix it was testing, but about
the conditions the user actually uses.

## What the smokes did vs. what users do

Every wmterm smoke launches the shell pipeline like this:

```
wmd 60 --clean &
wmterm -v 50          # NOTE: foreground — no & — see comment below
```

The comment from session 157's smoke was explicit:

```python
# IMPORTANT: foreground wmterm so the outer shell sits in
# sys_wait() and doesn't steal keystrokes from the kbd ring.
```

"Foreground" was load-bearing.  When wmterm is in foreground, the
outer shell is blocked in `sys_wait`, not touching the keyboard
ring.  wmd is the sole consumer.  Keystrokes route cleanly.

Users don't read smoke comments.  Users do `wmterm &` because
they want to keep using the outer shell.  Then the outer shell
is at its prompt, blocked in `keyboard_wait_char` (via
`tty_read`), pulling bytes off the same ring wmd polls.  The
race is purely up to the scheduler.

## The shared ring

Both consumers drain the same producer:

```
USB-HID keyboard ─┐
PS/2 keyboard    ─┼──► keyboard_inject ──► kbd_buf ring
COM1 serial      ─┘                          │
                                             ├──► keyboard_wait_char (tty_read)
                                             └──► sys_kbd_poll (wmd)
```

Whoever calls first wins each byte.  No fairness, no routing
intent — it's a race between two independent loops.

## The fix

Add a one-task grab.  When set, `keyboard_wait_char` yields any
task that isn't the grabber, so the outer shell's read effectively
stalls while wmd holds the grab.  wmd grabs on focus-in to a
client window, releases on focus-out.  The kernel auto-releases
on grabber exit so a crashed wmd doesn't lock the system out.

### Kernel

```c
/* kernel/keyboard.c */
static int g_kbd_grab_pid = 0;

char keyboard_wait_char(void) {
    for (;;) {
        keyboard_poll_once();
        serial_poll_once();

        struct task *t = task_current();
        if (g_kbd_grab_pid != 0 && t && (int)t->id != g_kbd_grab_pid) {
            task_yield();
            continue;
        }
        /* ...drain ring as before... */
    }
}

void keyboard_grab(int pid) { g_kbd_grab_pid = pid; }
int  keyboard_grabbed_by(void) { return g_kbd_grab_pid; }
```

```c
/* kernel/task.c, in task_exit_current */
if (keyboard_grabbed_by() == (int)t->id) keyboard_grab(0);
```

```c
/* kernel/syscall.c — new SYS_KBD_GRAB (109) */
case SYS_KBD_GRAB:
    if ((int)a) keyboard_grab((int)task_current()->id);
    else        keyboard_grab(0);
    ret = 0;
    break;
```

### wmd — grab only when a client is focused

The first cut had wmd grab unconditionally at startup.  That
broke the very next thing the user does after `wmd &`: typing
`wmterm` to the outer shell.  wmd had grabbed but had no focused
client, so the keystrokes were stuck — neither wmd nor the shell
could consume them.

The fix: grab on the focus edge.  wmd already tracks
`new_focus_id` for FOCUS / UNFOCUS event delivery; we piggyback
on that loop:

```c
int want_grab = (new_focus_id != 0);
if (want_grab != kbd_grabbed) {
    sys_kbd_grab(want_grab);
    kbd_grabbed = want_grab;
}
```

Now the lifecycle is:

| State                              | Grab? | Outer shell      |
|------------------------------------|-------|------------------|
| Just booted, wmd running, no focus | no    | reads kbd        |
| User types `wmterm &` in sh        | no    | gets keystrokes  |
| User clicks wmterm body            | yes   | blocks in waitch |
| User types into wmterm             | yes   | starves          |
| User clicks empty desktop          | no    | resumes          |

The user gets an intuitive model: clicking into a window gives
that window your keyboard, clicking out gives it back to the
host shell.

## Smoke

`smoke_wmterm_bg.py` launches wmterm with `&` — the previously
broken pattern — and verifies all five rungs of the chain:

| Check                                          | Source       |
|------------------------------------------------|--------------|
| shell banner reached wmterm (`rd n=`)          | trace        |
| wmd → wmterm FOCUS event delivery              | trace        |
| KEY 'a' routed to wmterm                       | trace        |
| PTY echo round-trip 'a' → wmterm               | trace        |
| outer shell readable again after click-off     | serial echo  |

The last one is the regression guard: if the grab leaked past
focus-out (or we lost the auto-release on task exit), the outer
shell would never get its kbd back.  The smoke sends a sentinel
command over serial after clicking off, watches for the echo.

Sessions 157, 158, 159 smokes still pass — no regression.  Those
all use foreground wmterm, so their outer shells are in
`sys_wait` and never racing; the grab is invisible to them.

## What I learned

This bug shipped because the smoke tested **a working
configuration** rather than **the configuration users use**.
Foreground vs background looked like an irrelevant launching
detail; it turned out to be the entire difference between "the
fix works" and "users still can't type".

For input plumbing in particular, the test fixture has to drive
the system the way a human does — not the way that happens to
sidestep the race condition you're not testing for.

## What changed, exhaustively

- `kernel/keyboard.h` — `keyboard_grab(int)` /
  `keyboard_grabbed_by(void)` decls.
- `kernel/keyboard.c` — `g_kbd_grab_pid` static, yield-other-tasks
  branch in `keyboard_wait_char`, accessor implementations.
- `kernel/task.c` — auto-release in `task_exit_current` if the
  dying task was the grabber.
- `kernel/syscall.{h,c}` — new `SYS_KBD_GRAB` (109).
- `user/libuser.{h,c}` — `sys_kbd_grab(int)` wrapper.
- `user/wmd.c` — edge-driven grab/release tied to focus state.
- `smoke_wmterm_bg.py` — new smoke for the `wmterm &` path.

## What this *doesn't* fix

- **Serial input still routes through the kbd ring.**  When wmd
  has a focused client, bytes coming in on COM1 also go to the
  client (the smoke trace makes this visible — typing a command
  over serial while wmterm was focused made the command land
  inside wmterm's inner sh, not the outer shell).  That's the
  current AdventOS design (serial_poll_once → kbd_inject); a
  proper fix is to route serial to its own ring, but that's a
  separate session.

- **No keyboard policy for non-WM-aware programs.**  A program
  that uses `sys_kbd_poll` directly (not through a wmd-routed
  WM_EV_KEY event) still gets a race-free read because we only
  yield in `keyboard_wait_char`, not in `keyboard_getc`.  Today
  only wmd uses `sys_kbd_poll`, so this is invisible; if another
  program ever starts polling kbd directly, it'll need its own
  grab story.
