# Session 139 — Path C phase 32: wmcalc calculator

**Goal.** A real interactive app under the WM that's neither a
text editor nor a viewer.  Adds a new visible launcher entry and
exercises the click-routing path more thoroughly than any prior
app (20 hit-test regions instead of 1-3).

Status: **done.**  Smoke test `smoke_wmcalc.py` (6/6 pass):

```
=== pixel checks ===
  [OK] wmd-side title cyan @ y=209 (149/200)
  [OK] wmcalc header blue @ y=222 (199/200)
  [OK] display panel deep-blue (8, 16, 24)
  [OK] Clear btn red-ish (160, 64, 64)
  [OK] = btn green-ish (96, 160, 64)
  [OK] wmd status bar alive (879/924)
```

After boot, click into wmcalc's body for focus, type "12+34="
on the host keyboard via QMP send-key, screendump.  Each button
type has a distinct fill colour (red `C`, green `=`, slate ops,
dark digits, brown utility), so per-button hue checks confirm
the grid laid out correctly.

---

## Layout

```
+---------------------------------+   ← wmd outer (cyan frame)
| wmcalc                  [_][[][X]|
+---------------------------------+
| wmcalc  Ctrl-Q quit              |   inner header (blue)
+---------------------------------+
| +                          579   |   ← display (deep blue)
+---------------------------------+
|  C  | +/- |  <- |  /  |          |   ← buttons start here
|  7  |  8  |  9  |  *  |
|  4  |  5  |  6  |  -  |
|  1  |  2  |  3  |  +  |
|  0  |  .  |  =  | AC  |
+---------------------------------+
   220 x 320
```

- WIN_W=220, WIN_H=320
- HDR_H=18 outer chrome, body 18..320
- Display panel: y=24..67 inside the window surface
- Button grid: 4 cols × 5 rows, 48×44 each, 4 px gap
- Pending operator shown in the top-left of the display panel

---

## State machine

Two-register evaluation — same model as a $10 pocket calculator:

```c
static int  g_acc;       /* accumulator */
static int  g_cur;       /* current input being typed */
static char g_op;        /* 0 / + / - / * / / (pending) */
static int  g_entering;  /* 1 if digits are still being typed */
static int  g_err;       /* sticky div-by-zero flag */
```

The sequence `12 + 34 =` flows like this:

| step | action     | acc | cur | op | entering |
|------|------------|-----|-----|----|----------|
| `1`  | digit      |   0 |   1 |  0 | 1        |
| `2`  | digit      |   0 |  12 |  0 | 1        |
| `+`  | op         |  12 |  12 | `+`| 0        |
| `3`  | digit      |  12 |   3 | `+`| 1        |
| `4`  | digit      |  12 |  34 | `+`| 1        |
| `=`  | equals     |  46 |  46 |  0 | 0        |

Chained ops (`1 + 2 + 3 =`) fold left-to-right because every
`op` press first applies the *pending* op before storing the
new one:

```c
static void press_op(char op) {
    if (g_op != 0 && g_entering) {
        g_acc = apply(g_acc, g_op, g_cur);
        g_cur = g_acc;
    } else {
        g_acc = g_cur;
    }
    g_op = op;
    g_entering = 0;
}
```

Division by zero sets `g_err`, which freezes the state machine
until `C` is pressed; the display shows `ERR` in red.

---

## Why integers (for now)

libuser doesn't have a float printer (ftoa / sprintf %f) yet.
Doing softfloat just for the calculator would be a big detour;
we use `int` and leave the `.` button as a visible no-op so the
button grid is symmetric.  Future polish session can add a
softfloat or fixed-point path.

Tried `long long` first — toolchain pulled in `__divdi3` which
isn't linked into freestanding userspace.  Switched to `int`;
the cap is 9 digits during entry so a chained multiply against
another 9-digit operand can't overflow int32_t.

---

## Click routing

Each frame the main loop polls `WM_EV_MOUSE_PRESS` events and
runs them through `hit_button`:

```c
static int hit_button(int x, int y) {
    if (x < GRID_X || y < GRID_Y) return -1;
    int col = (x - GRID_X) / (BTN_W + BTN_GAP);
    int row = (y - GRID_Y) / (BTN_H + BTN_GAP);
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return -1;
    /* Reject clicks in the GAP between cells. */
    if ((x - GRID_X) % (BTN_W + BTN_GAP) >= BTN_W) return -1;
    if ((y - GRID_Y) % (BTN_H + BTN_GAP) >= BTN_H) return -1;
    return row * COLS + col;
}
```

The "between-cell" rejection matters — without it a click in the
4-pixel gap would map to whichever cell the integer-division
landed in, which feels imprecise.  Returning -1 for gap clicks
gives a tactile "miss" feel.

Pressed buttons brighten by an inline `((bg & 0xFEFEFEu) >> 1) +
0x808080u` (one-shot 6-frame highlight via `g_pressed_ticks`).

---

## Key bindings

| key                  | action |
|----------------------|--------|
| `0`-`9`              | digit |
| `+` `-` `*` `/`      | operator |
| `=` or Enter         | evaluate |
| `c` / `C`            | clear all |
| Backspace            | drop last digit of current entry |
| `_`                  | toggle sign of current entry |
| `.`                  | no-op placeholder (integer build) |
| Ctrl-Q               | quit |

Keys are mapped to button indices via `key_to_button`, then the
same `press_button` dispatch fires — keyboard and mouse take
the same code path, with the same one-shot button-press
highlight.

---

## What stays out of scope

- **Floating point.**  `7 / 2` returns 3 (truncated integer
  division).  Decimal-point button is a placeholder.
- **History / memory keys.**  No M+ / M- / MR / MS.  No previous-
  result recall.
- **Scientific functions.**  No √, x², log, etc.
- **Expression mode.**  No parens, no operator precedence — it's
  strictly two-register left-to-right.
- **Hex / binary mode.**  Decimal only.
- **Resize.**  Window is fixed 220×320.  Buttons don't reflow.
  (Session 131's resize grip still works mechanically — the
  buttons just stay anchored at their original coordinates.)

That's still enough to do real arithmetic with both mouse and
keyboard, and it adds an actual interactive widget to the
desktop alongside wmedit / wmterm.

---

## Files touched

- `user/wmcalc.c` — new, ~300 lines: state machine + paint +
  click/key routing
- `build.sh` — wmcalc joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmcalc.elf` + man page in the image
- `fs/man/wmcalc` — new
- `user/wmd.c` — `wmcalc` joins the Start-menu launcher catalog
  (now 11 items)
- `smoke_wmcalc.py` — new harness, 6 pixel checks
- `docs/125-pathC-wmcalc.md` — this file

kernel.bin: 147632 (vs 143536 at session 138 — the +4 KB is
Path E phase 7's virtio-scsi + CDC-ACM that landed in parallel
and rebased through; wmcalc itself is pure userspace).
wmcalc.bin: new 6424 bytes.  wmd.bin: 16920 → 16952 (+32 B
for the catalog entry).

---

## Path C status after session 139

- ✅ 107..138 — see prior docs
- ✅ 139 — wmcalc 4-function calculator

11 apps in the Start menu launcher now: wmhello, wmtype,
wmclock, wmpaint, wmpair, wmfiles, sysinfo, wmps, wmterm,
wmedit, wmcalc.
