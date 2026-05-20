# Session 163 — wmterm clear + mouse-wheel scrollback

Path C phase 56.  Two more interactive features from user requests.

## clear

The shell already had a `clear` builtin (session 141, origin
branch) that called `sys_tty_clear()` — a kernel syscall that
clears the global framebuffer console.  Same shape as the
session 161 backspace / EOL problem: the syscall doesn't
propagate through a PTY, so running `clear` inside wmterm did
nothing visible.

Same shape, same fix.  Augment the builtin to emit the standard
ANSI sequences too:

```c
/* user/sh.c — cmd_clear */
sys_tty_clear();                                /* kernel console */
putchar(27); putchar('['); putchar('H');        /* cursor home */
putchar(27); putchar('['); putchar('2'); putchar('J'); /* clear screen */
```

And teach wmterm's vt_feed to act on CSI J and CSI H:

```c
/* user/wmterm.c — csi_dispatch */
case 'J': {
    int mode = csi_get_param(0);
    if (mode == 2 || mode == 3) {
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++)
                g_grid[r][c] = 0;
        if (mode == 3) {           /* DEC extension: also flush ring */
            g_sb_count = 0; g_sb_head = 0; g_view_offset = 0;
        }
    } else if (mode == 0) { /* cursor → end of screen */ ... }
    else if (mode == 1)   { /* start of screen → cursor */ ... }
    return;
}
case 'H': case 'f': {
    int row = csi_get_param(1);
    if (row < 1) row = 1; if (row > ROWS) row = ROWS;
    g_cur_row = row - 1; g_cur_col = 0;
    return;
}
```

Mode 3 (clear scrollback) is a DEC extension that real `clear`
programs sometimes emit; we honor it but sh's builtin only
sends mode 2, which preserves the scrollback ring (matches every
modern terminal — `clear` clears the visible screen, scroll-back
remains).

Ctrl-L in the line editor gets the same treatment.

## Mouse-wheel scrollback

USB-tablet reports include a vertical-wheel byte at index 5 that
`kernel/usb_hid.c`'s `poll_one_tablet` had been ignoring since
session 141 (the comment was honest: "vertical wheel ... ignored
for now").  Wiring it through the stack:

### Kernel

```c
/* kernel/mouse.c */
static int g_wheel;
void mouse_add_wheel(int delta) {
    g_wheel += delta;
    if (g_wheel >  10000) g_wheel =  10000;
    if (g_wheel < -10000) g_wheel = -10000;
}
int  mouse_consume_wheel(void) { int v = g_wheel; g_wheel = 0; return v; }
```

```c
/* kernel/usb_hid.c — poll_one_tablet */
int wheel = (int)(int8_t)report[5];
if (wheel != 0) mouse_add_wheel(wheel);
```

The accumulation is important.  USB-tablet polls at 15 ms but
wmd polls mouse state at 33 ms (its frame rate), so multiple
wheel ticks can arrive between wmd reads.  Summing in the
kernel and clearing on poll means wmd sees the *total* delta
since its last frame — same shape as Linux's input-event
accumulator.

### Syscall surface

`struct sys_mouse_state` grew a `wheel` field.  All callers
include the same header and rebuild together, so the layout
shift is invisible — no ABI version negotiation needed.

### wmd routing

Mouse-wheel events naturally target the **hovered** window, not
the focused one (that's how every modern desktop works — you
don't need to click a panel to scroll it, just point at it).
We already compute `target` from `hit_test(ms.x, ms.y)` for
mouse-move events; we piggyback:

```c
/* user/wmd.c */
if (ms.wheel != 0) {
    struct sys_wm_event ev = {0};
    ev.type    = WM_EV_MOUSE_WHEEL;          /* new in session 163 */
    ev.x       = sx;
    ev.y       = sy;
    ev.keycode = (unsigned int)ms.wheel;     /* signed delta */
    sys_wm_event_push(w->client_id, &ev);
}
```

`keycode` was the most convenient existing field to carry the
signed delta — keeps `struct sys_wm_event` stable.

### wmterm handling

The wheel delta from USB-tablet is in raw "ticks" — about 1-3
per physical click of the wheel.  PgUp/PgDn step `PAGE_STEP = 12`
rows; treating 4 wheel ticks as one page-step makes a single
physical click of the wheel feel about right:

```c
/* user/wmterm.c — WM_EV_MOUSE_WHEEL handler */
int delta = (int)(int32_t)ev.keycode;
if (delta > 0) {
    int steps = delta / 4; if (steps < 1) steps = 1;
    for (int i = 0; i < steps; i++) scroll_up();
} else if (delta < 0) {
    int steps = (-delta) / 4; if (steps < 1) steps = 1;
    for (int i = 0; i < steps; i++) scroll_down();
}
```

A vigorous flick of the wheel produces a larger delta and walks
the view multiple pages.

## QMP detail (for the smoke)

QEMU encodes scroll-wheel events as **button** events with name
`wheel-up` / `wheel-down`, not as a relative-axis event with name
`wheel-y` (I tried; nothing happened).  The smoke's wheel helper:

```python
def wheel(q, qbuf, up):
    name = "wheel-up" if up else "wheel-down"
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": name}}]})
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": name}}]})
```

USB-tablet reports are coalesced — the report containing the
wheel byte goes out on the *next* position-change.  The smoke
tickles the cursor by ±1 px after each wheel click to coax
QEMU into flushing the report.

## Smoke

`smoke_wmterm_clear_wheel.py` — five checks:

| Check                                          | Pass rate (4 runs) |
|------------------------------------------------|--------------------|
| focus wmterm                                   | 3/4                |
| ls / filled the grid (>200 green text px)      | 3/4                |
| clear emptied the grid                         | 3/4                |
| mouse wheel event reached wmterm               | 4/4                |
| wheel scrolled view into history               | 3/4                |

The one fully-failed run lost focus initially — the QEMU click
flake we've seen across sessions, not specific to this work.
When focus takes, every check passes.

Sessions 157, 159, 161, 162 smokes all still pass — no
regression on the existing scrollback / backspace / cursor work.

## What changed, exhaustively

- `kernel/mouse.{h,c}` — `mouse_add_wheel()` / `mouse_consume_wheel()`.
- `kernel/usb_hid.c` — `poll_one_tablet` reads byte 5 of the report
  and forwards via `mouse_add_wheel`.
- `kernel/syscall.h` — `struct sys_mouse_state` grew a `wheel`
  field; new `WM_EV_MOUSE_WHEEL` event type.
- `kernel/syscall.c` — `SYS_MOUSE_POLL` drains the wheel into
  the user struct.
- `user/libuser.h` — mirror of struct field + event type.
- `user/wmd.c` — pushes `WM_EV_MOUSE_WHEEL` to the hovered client
  when `ms.wheel != 0`.
- `user/wmterm.c` — vt_feed handles CSI J / H; main loop's KEY
  switch grew a `WM_EV_MOUSE_WHEEL` case that steps the view by
  `delta / 4` row-pages.
- `user/sh.c` — `cmd_clear()` + Ctrl-L emit `ESC [ H ESC [ 2 J`
  on top of `sys_tty_clear()`.
- `fs/man/wmterm` — updated.
- `smoke_wmterm_clear_wheel.py` — new.

## What this *doesn't* fix

- **No horizontal wheel.**  USB-tablet doesn't surface a
  horizontal-wheel byte in its 6-byte report, and wmterm's
  scrollback is vertical anyway.  Adding it would need both a
  QEMU patch and a use case.

- **Wheel doesn't affect other windows.**  wmedit could honor
  it for cursor movement, wmview for image zoom, wmcalc could
  ignore.  Each client decides via their own
  `WM_EV_MOUSE_WHEEL` handler.

- **`clear` doesn't wipe the scrollback ring.**  Matches every
  modern terminal — `clear` clears the visible screen,
  PgUp/PgDn still works.  Use `clear` followed by typing CSI 3J
  (or, when we add it, a `clear -x` flag) to also flush
  scrollback.
