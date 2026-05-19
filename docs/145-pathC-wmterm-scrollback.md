# Session 159 — wmterm scrollback (PgUp / PgDn)

Path C phase 52.  With wmterm's input + close now solid (sessions
157, 158), the obvious next thing the user reaches for is "let me
look at what scrolled off the top".

## What this adds

A 500-row ring buffer behind wmterm's visible 24-row grid.  Every
time `grid_scroll()` pushes the top row off, that row goes into
the ring instead of disappearing.  PgUp/PgDn step the view 12 rows
into history / back toward live; any new shell output snaps the
view back to the live tail (matches every modern terminal).

Three layers had to cooperate.

### Layer 1 — kernel emits ANSI CSI for PgUp / PgDn

The existing HID-to-ASCII table maps usages 0x4F..0x52 (arrows) to
the 3-byte ANSI CSI sequences `ESC [ A/B/C/D`.  Following that
convention, PageUp (0x4B) and PageDown (0x4E) now emit the 4-byte
`ESC [ 5 ~` and `ESC [ 6 ~`:

```c
/* kernel/usb_hid.c */
if (usage == 0x4B) {            /* PageUp */
    char esc[4] = { 27, '[', '5', '~' };
    keyboard_inject(esc, 4);
    return;
}
if (usage == 0x4E) {            /* PageDown */
    char esc[4] = { 27, '[', '6', '~' };
    keyboard_inject(esc, 4);
    return;
}
```

These flow through the same `keyboard_inject` pipe as every other
keystroke — so they show up in the kbd ring, get drained by wmd
each frame, and arrive at the focused client as four separate
`WM_EV_KEY` events with keycodes 0x1B, 0x5B, 0x35, 0x7E.

### Layer 2 — sh.c parses CSI parameter bytes properly

A side-effect of emitting these for kernel-console use: the
kernel-console shell now sees `ESC [ 5 ~` when the user presses
PgUp.  The old `read_line_interactive` CSI parser read exactly
three bytes (`ESC '[' final`) and would have:

1. Consumed ESC.
2. Consumed '['.
3. Consumed '5' as the "final byte", not recognised → dropped.
4. Then the *next* loop iteration would read '~' as a normal
   character and insert it into the line.

Fixed:

```c
/* user/sh.c — between '[' and the final, consume any parameter
 * or intermediate bytes (0x20..0x3F).  '5' is a parameter byte,
 * so we loop until we hit the final '~' and drop the whole
 * sequence cleanly. */
while ((unsigned char)b >= 0x20 && (unsigned char)b <= 0x3F) {
    char nxt;
    if (sys_read(0, &nxt, 1) <= 0) break;
    b = nxt;
}
```

This matches the ECMA-48 / ANSI X3.64 CSI grammar.  Unknown
sequences are still dropped, but their tails (the `~` of `5~`)
no longer leak into the input buffer.

### Layer 3 — wmterm scrollback ring + input CSI parser

The new state:

```c
#define SB_ROWS    500
#define PAGE_STEP  12               /* half a screen */
static char g_sb[SB_ROWS][COLS];
static int  g_sb_count;             /* 0..SB_ROWS */
static int  g_sb_head;              /* oldest row index */
static int  g_view_offset;          /* rows scrolled back */
```

`grid_scroll()` now copies `g_grid[0]` into the ring before
shifting:

```c
int slot = (g_sb_head + g_sb_count) % SB_ROWS;
for (int c = 0; c < COLS; c++) g_sb[slot][c] = g_grid[0][c];
if (g_sb_count < SB_ROWS) g_sb_count++;
else g_sb_head = (g_sb_head + 1) % SB_ROWS;
```

Rendering goes through a `visible_row(r)` helper that walks the
union of (scrollback + current grid) and picks the right row for
each visible window position:

```c
int abs_row = (g_sb_count - g_view_offset) + r;
if (abs_row < g_sb_count) return g_sb[(g_sb_head + abs_row) % SB_ROWS];
return g_grid[abs_row - g_sb_count];
```

The blinking caret hides when `g_view_offset > 0` so the user
doesn't see a cursor blinking somewhere unrelated to what they're
looking at.

The header label swaps in history mode:

```
wmterm - sh.elf                            (live, focused)
wmterm - history - 36 rows (PgDn to live)  (scrolled back)
```

### Layer 3, harder bit — input-side CSI parser

A single PgUp arrives as **four** `WM_EV_KEY` events.  wmterm
has to buffer them, detect the special sequence, and either
consume it locally or forward all four bytes to the PTY so e.g.
arrow keys still reach sh.elf for history navigation.

State machine in `key_byte()`:

| State (len) | Incoming byte    | Action                              |
|-------------|------------------|-------------------------------------|
| 0           | ESC (0x1B)       | buffer, len=1, return               |
| 0           | anything else    | write to PTY, return                |
| 1           | `[`              | buffer, len=2, return (CSI started) |
| 1           | anything else    | flush `ESC + b` to PTY, reset       |
| ≥2          | 0x20..0x3F       | buffer (parameter), return          |
| ≥3          | 0x40..0x7E       | sequence complete — dispatch        |
| else        | anything         | flush + reset (don't lose bytes)    |

On dispatch:
- `len == 4 && buf == ESC [ 5 ~` → `scroll_up()`
- `len == 4 && buf == ESC [ 6 ~` → `scroll_down()`
- otherwise → forward the whole CSI to the PTY

`scroll_up` advances `g_view_offset` by `PAGE_STEP`, clamped to
`g_sb_count` (can't scroll above the oldest row in the ring).
`scroll_down` decrements, clamped to 0.

Snap-to-live is the simplest possible: any time `sys_read(master,
...)` returns `> 0`, we set `g_view_offset = 0` before feeding
the bytes into `vt_feed`.

### A subtle bug I shipped + fixed

First cut of `key_byte()` had a fall-through bug: when `len == 2`
and the byte is `[` (the valid CSI prefix), none of the explicit
branches matched and the code reached the trailing
"flush + reset" path.  Result: `ESC[` flushed straight to the PTY,
the `5` and `~` followed as plain bytes, sh ate them all as
"unknown CSI parameter", no scroll, no error visible to the user.

Fixed by adding an explicit `if (g_kbd_esc_len == 2 && b == '[')
return;` so the CSI start is treated as "keep collecting" rather
than falling through.  The trace-driven smoke caught this on the
first run — `view=0` everywhere, even on the final `~` of the
sequence.

## Memory cost

`g_sb` is 500 × 60 = 30 KB.  User binaries fold `.bss` into
`.data` so this shows up in the on-disk `wmterm.bin` size (jumped
from 6.5 KB to ~37.5 KB), but that's still under any cap and
well below the 256 KB cell the kernel ELF loader allocates per
user binary.

A circular buffer is the right shape here — fixed footprint,
amortized O(1) push, O(1) random access for the renderer.  At
500 rows we keep maybe 20× the visible viewport, plenty for the
"scroll back to see what `ls` printed" use case.

## Smoke

`smoke_wmterm_scrollback.py` is trace-driven, with pixel
verification as a cross-check:

| Check                                          | Source       |
|------------------------------------------------|--------------|
| shell output reached wmterm (`rd n=` in trace) | trace        |
| PgUp scrolled into history (view > 0 in trace) | trace        |
| history-mode title visible (pixel diff)        | screenshot   |
| ESC byte routed to wmterm via KEY event        | trace        |
| PgDn snapped back to live (view == 0 in trace) | trace        |

The trace-based checks are rock solid (5/5 across runs).  The
pixel check is the only flaky one — sometimes the screenshot
catches wmterm mid-repaint and the longer "history - N rows..."
label hasn't yet replaced the live label.  Adequate as a
cross-check but the trace-based checks are the real verification.

Sessions 157 + 158 smokes still pass — no regression.

## What changed, exhaustively

- `kernel/usb_hid.c` — HID usages 0x4B/0x4E emit `ESC[5~` / `ESC[6~`.
- `user/sh.c` — `read_line_interactive` consumes CSI parameter
  bytes (0x20..0x3F) between `[` and the final, so unknown
  sequences don't dribble tail bytes into the input line.
- `user/wmterm.c` — 500-row scrollback ring, `g_view_offset`,
  `visible_row()` accessor, input-side CSI state machine,
  snap-to-live on shell output, header label swap, caret hide
  in history mode.
- `fs/man/wmterm` — documented the new keys + history note.
- `smoke_wmterm_scrollback.py` — new smoke.

## What this *doesn't* fix

- **No mouse wheel scroll**.  USB-tablet reports do include a
  vertical-wheel byte (`report[5]`) that kernel/usb_hid.c
  currently ignores.  Wiring it through `mouse_set_absolute` →
  a new field → wmd → a new WM event would let the user scroll
  wmterm history with the wheel.  Not done here.

- **Selection / copy**.  Once you can see history, you'll want
  to copy text out of it.  Scrollback is the prerequisite; the
  selection state machine is a separate piece of work that
  needs to interact with the clipboard syscall from session 122.

- **Search inside scrollback**.  `wmedit` has Ctrl-F now
  (session 152); a similar mini-prompt for "find in history"
  in wmterm would be natural.  Out of scope here.
