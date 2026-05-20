# Session 168 — four more polish items in one session

Path C phase 61.  The second batch of items the previous docs
flagged as deferred:

| # | Item                                              | Source doc |
|---|---------------------------------------------------|------------|
| 1 | Mid-line caret position inside wmterm             | docs/147  |
| 2 | wmedit mouse wheel scroll                         | docs/149  |
| 3 | wmview mouse wheel pan                            | docs/149  |
| 4 | Keyboard-driven selection (Shift+arrow / Home / End) | docs/151  |
| 5 | Middle-click paste                                | docs/151  |

(Five items — the user asked for four but the wmedit / wmview
wheel split into two tiny pieces.)

## 1. Mid-line caret via CSI G

sh.elf's `position_cursor(prompt_row, want_col)` used to call
only `sys_tty_cursor(prompt_row, prompt_len() + want_col)` — a
kernel-console syscall that doesn't propagate through a PTY.
Inside wmterm the caret stayed at end-of-line for every
mid-line edit (Ctrl-A, Ctrl-B, Ctrl-F, backspace mid-word,
history navigation).

Fix: emit ANSI `CSI <col> G` (Cursor Horizontal Absolute) right
after the syscall.  Wmterm's `vt_feed` grew a 'G' handler that
sets `g_cur_col = col - 1` without touching the row.

Trace verification: typing `abc` then Ctrl-A produces this
sequence in the wmterm read preview:

```
wmterm: rd n=22 first=0x61 [abc.advent$ abc.[K.[9G]
                                          ^^^^   <-- CSI K + CSI 9G
```

CSI 9 G = "go to column 9", which is right after the 9-char
`advent$ ` prompt.  The caret blinks there instead of after the
`abc`.

## 2. wmedit mouse wheel scroll

`WM_EV_MOUSE_WHEEL` handler decremented `g_scroll_row` by
`delta / 4` per tick.  The catch: wmedit's per-frame
"keep cursor in view" clamp would immediately snap the view
back to the cursor.  Added a `g_view_manual` flag set by the
wheel handler and cleared by any KEY event — while it's set,
the auto-scroll clamp is skipped.  Standard editor behaviour:
the user can wheel-scroll past the cursor; typing snaps the
view back.

## 3. wmview mouse wheel pan

wmview previously painted the image once before entering the
event loop and only re-painted the title bar on each frame.
Refactored: the image blit moved into the per-frame paint,
gated on a `g_pan_y` offset.  Wheel handler adjusts the offset
by 8 px per tick, clamped to keep the image at least partially
visible.

## 4. Keyboard-driven selection

Real xterm emits the modified-CSI form `ESC [ 1 ; 2 X` when
Shift is held alongside an arrow / Home / End.  The kernel HID
+ PS/2 paths now do the same:

```c
/* kernel/usb_hid.c */
if (shift) {
    char esc[6] = { 27, '[', '1', ';', '2', final };
    keyboard_inject(esc, 6);
} else {
    char esc[3] = { 27, '[', final };
    keyboard_inject(esc, 3);
}
```

PS/2 set-1 scancodes 0x47 / 0x4F (Home / End) got the same
treatment as the arrows.

wmterm's `key_byte` CSI parser learned to recognise the 6-byte
form and call `sel_kbd_extend(direction)`:

```c
static void sel_kbd_extend(char direction) {
    if (!sel_active()) {
        /* Seed anchor at the shell cursor (the blinking caret). */
        g_sel_anchor_row = g_sb_count + g_cur_row;
        g_sel_anchor_col = g_cur_col;
        g_sel_head_row   = g_sel_anchor_row;
        g_sel_head_col   = g_sel_anchor_col;
    }
    switch (direction) {
        case 'A': g_sel_head_row--; break;
        case 'B': g_sel_head_row++; break;
        case 'C': /* right + line wrap */ break;
        case 'D': /* left  + line wrap */ break;
        case 'H': g_sel_head_col = 0; break;
        case 'F': g_sel_head_col = COLS - 1; break;
    }
    /* Auto-copy each step so the user can paste mid-stride. */
    char cbuf[2048];
    int n = sel_copy_text(cbuf, sizeof(cbuf));
    if (n > 0) sys_clipboard_set(cbuf, n);
}
```

The selection anchor seeds at the shell's current cursor on
first Shift+arrow press, then each subsequent press moves the
head.  No separate "selection caret" needed — the existing
PTY-driven caret is the start point.

## 5. Middle-click paste

X11-style "middle-click pastes the primary selection."

wmd grew a `g_prev_middle` edge-tracker and dispatches
`WM_EV_MOUSE_PRESS` with `ev.button = WM_BUTTON_MIDDLE` when
the middle button presses inside a client window.  No paired
RELEASE — middle-click is one-shot, matches X11 / Windows.

wmterm's existing `WM_EV_MOUSE_PRESS` handler grew a
middle-button branch:

```c
if (ev.button & WM_BUTTON_MIDDLE) {
    char pbuf[1024];
    int pn = sys_clipboard_get(pbuf, sizeof(pbuf));
    if (pn > 0) sys_write(master, pbuf, pn);
    break;
}
```

Same payload as Ctrl-V; just a different trigger.

## QEMU caveat (familiar by now)

QEMU's usb-tablet device doesn't emit a fresh report for a
position-only event while a button state is unchanged.  We
hit this in session 165 for left-button drag and in session
166 for keyboard injection.  The middle-click smoke uses the
same workaround: bundle the abs coords AND the button-down in
a single `input-send-event` so QEMU emits one combined report.
With that change the smoke is 3/3 reliable.

## Smoke

`smoke_wmterm_polish_168.py` — four checks, 3/3 fresh-QEMU runs
pass:

| Check                                          |  3/3  |
|------------------------------------------------|-------|
| sh.elf emits CSI G for mid-line caret          |  OK   |
| Shift+Right CSI sequence routed to wmterm      |  OK   |
| middle-click delivered as PRESS btn=4          |  OK   |
| MPASTE intercept fired                         |  OK   |

wmedit + wmview wheel handlers are exercised by the existing
session-163 wheel infrastructure and verified by code review;
no dedicated smoke (the screenshots would need a separate
file-based test fixture).

Sessions 157-167 smokes all still pass — no regression on the
existing wmterm / wmd functionality.

## What changed, exhaustively

- `user/sh.c` — `position_cursor` also emits `CSI <col> G`.
- `user/wmterm.c`:
  - `csi_dispatch` case 'G' (Cursor Horizontal Absolute).
  - `sel_kbd_extend` for Shift+arrow / Home / End.
  - `key_byte` CSI parser recognises the 6-byte modified form.
  - `WM_EV_MOUSE_PRESS` middle-button branch pastes from
    clipboard.
- `kernel/usb_hid.c` — Shift+arrow + Home / End + Shift+Home /
  End emit modified CSI.
- `kernel/keyboard.c` — same for the PS/2 set-1 path; Home
  (sc 0x47) and End (0x4F) added (previously dropped).
- `user/wmd.c` — `g_prev_middle` edge tracker + MOUSE_PRESS
  dispatch for middle button.
- `user/wmedit.c` — `WM_EV_MOUSE_WHEEL` handler + `g_view_manual`
  flag so auto-scroll doesn't snap back.
- `user/wmview.c` — `WM_EV_MOUSE_WHEEL` handler + per-frame
  image re-blit + `g_pan_y` offset.
- `smoke_wmterm_polish_168.py` — new.

## What this *doesn't* fix

- **Shift+selection across long wraps.**  `sel_kbd_extend`'s
  C/D directions know about the COLS boundary and wrap to
  prev/next row, but the SHELL's cursor doesn't follow that
  movement — only the selection head does.  Fine for picking
  a few words; weird if you Shift+Right across many rows.

- **wmview's wheel doesn't zoom.**  Standard image viewers
  use wheel for zoom; we use it for pan.  Zoom requires per-
  pixel interpolation that the current bitmap-blit path
  doesn't do.  Pan is the smaller change.

- **wmedit's wheel doesn't scroll horizontally.**  Long lines
  past the right edge can't be reached by wheel.  Add an
  Shift+wheel or horizontal-wheel binding if it matters.
