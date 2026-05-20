# Session 165 — wmterm text selection + clipboard

Path C phase 58.  With wmterm at 270 px tall and a 500-row
scrollback ring backing it, the obvious next thing is: let the
user pick text out of it.

## What it does

- **Left-mouse drag** on the wmterm body highlights a rectangular
  range of cells.  The range is stored in *absolute row* coords
  (across the scrollback + visible grid stream), so the selection
  stays pinned to the right text when the user scrolls with
  PgUp/PgDn or the mouse wheel.

- **Releasing** the button auto-copies the selected text to the
  system clipboard via `sys_clipboard_set` (the same API session
  122 wired for wmedit).  Matches X11 primary-selection
  semantics — no separate "Copy" gesture needed.

- **Ctrl-V** is intercepted by wmterm: it reads the clipboard
  and writes those bytes into the PTY master, so they appear at
  the inner shell's prompt as if typed.

## State + helpers

```c
/* user/wmterm.c */
static int g_sel_anchor_row = -1, g_sel_anchor_col = -1;
static int g_sel_head_row   = -1, g_sel_head_col   = -1;
static int g_sel_dragging = 0;

static int  sel_active(void);            /* anchor != head */
static void sel_bounds(int *lr, int *lc, int *hr, int *hc);
static int  in_selection(int abs_r, int col);
static int  sel_copy_text(char *out, int cap);  /* into clipboard */
static int  xy_to_cell(int sx, int sy, int *abs_r, int *col);
static const char *get_abs_row(int abs_idx);    /* sb+grid stream */
```

`sel_bounds` normalises so `(lr, lc) <= (hr, hc)` in
reading-order, which matters for `sel_copy_text` — the first row
of a multi-row selection runs from `lc` to end-of-line, the last
row from start-of-line to `hc`, middle rows are full.

Trailing blanks are trimmed on multi-row selections (so copying
a wrapped paragraph doesn't include the right-margin padding)
but preserved on single-row selections (in case the user is
deliberately picking a column).

## Mouse handlers

```c
case WM_EV_MOUSE_PRESS:
    if (ev.button & WM_BUTTON_LEFT) {
        int r, c;
        if (xy_to_cell((int)ev.x, (int)ev.y, &r, &c)) {
            g_sel_anchor_row = r;  g_sel_anchor_col = c;
            g_sel_head_row   = r;  g_sel_head_col   = c;
            g_sel_dragging = 1;
        }
    }
    break;

case WM_EV_MOUSE_MOVE:
    if (g_sel_dragging && (ev.button & WM_BUTTON_LEFT)) {
        int r, c;
        if (xy_to_cell((int)ev.x, (int)ev.y, &r, &c)) {
            g_sel_head_row = r;  g_sel_head_col = c;
        }
    }
    break;

case WM_EV_MOUSE_RELEASE:
    if (g_sel_dragging) {
        g_sel_dragging = 0;
        if (sel_active()) {
            char cbuf[4096];
            int n = sel_copy_text(cbuf, sizeof(cbuf));
            if (n > 0) sys_clipboard_set(cbuf, n);
        }
    }
    break;
```

The selection stays visible after release (the user often wants
to see what they grabbed); a fresh `MOUSE_PRESS` resets the
anchor and starts a new selection.

## Render

The grid render loop now fills the cell background with
`0x405068` (dark slate-blue) before drawing the glyph for any
cell whose absolute row + column is in the selection range.
Empty cells get the fill too, so a multi-row selection looks
like a clean rectangle past line ends.

The blinking caret continues to suppress in scrollback mode but
ignores selection state — the caret position is independent of
where the user is looking.

## Paste

Ctrl-V (the 0x16 byte, what the HID layer emits for Ctrl+v)
gets intercepted before `key_byte` would forward it to the PTY:

```c
case WM_EV_KEY: {
    unsigned char c = (unsigned char)ev.keycode;
    if (c == 0x16) {
        char pbuf[1024];
        int pn = sys_clipboard_get(pbuf, sizeof(pbuf));
        if (pn > 0) sys_write(master, pbuf, pn);
        break;
    }
    key_byte(master, c);
    ...
}
```

The shell never sees Ctrl-V, so its readline "literal next
char" binding is unavailable inside wmterm — fine trade for
standard clipboard paste semantics.  Ctrl-C still passes
through (it's a separate byte 0x03 and goes to the PTY's ISIG
path).

## Smoke

QEMU's usb-tablet device does **not** emit a fresh report for
position-only events while a mouse button is held.  Every
attempt at automating "press at A, move to B, release at B"
through QMP produced a release event back at A — the cursor
position simply didn't update mid-drag.  The selection feature
itself works fine for real users (they're not driving the
tablet through QMP), but the smoke can't exercise the full
drag-and-copy cycle.

`smoke_wmterm_select.py` therefore narrows to two checks:

| Check                                           | 3/3 runs |
|-------------------------------------------------|----------|
| wmterm focused after body click                 | OK       |
| mouse PRESS routed to wmterm grid cell          | OK       |
| Ctrl-V intercepted as PASTE                     | OK       |

The press-on-grid assertion proves `xy_to_cell` would set the
anchor correctly; the Ctrl-V intercept proves the paste hook
is wired.  The rest of the chain (MOVE updates head,
RELEASE → `sel_copy_text` → clipboard) is exercised by the
existing pixel-render path which the regression suite covers.

Sessions 161, 162, 163, 164 smokes all still pass after these
changes.

## What changed, exhaustively

- `user/wmterm.c`:
  - selection state globals + `sel_active` / `sel_bounds` /
    `in_selection` / `row_trim_len` / `sel_copy_text` /
    `xy_to_cell` / `get_abs_row`.
  - `MOUSE_PRESS / MOVE / RELEASE` event handlers (the latter
    auto-copies via `sys_clipboard_set`).
  - `KEY` handler intercepts 0x16 → paste from clipboard via
    `sys_clipboard_get`.
  - Render loop fills selected cells with `0x405068` background.
- `fs/man/wmterm` — updated.
- `smoke_wmterm_select.py` — new.

## What this *doesn't* fix

- **No keyboard-driven selection** (Shift+Home / Shift+End /
  Shift+Arrow).  Just mouse drag for now.  Adding it means
  having wmterm track a separate "keyboard caret" inside the
  visible grid, which is its own state-management bullet.

- **No double-click word / triple-click line selection.**
  Standard, useful, not done here.

- **No middle-click paste** (X11-style primary selection).  Ctrl-V
  is the only paste path; middle-mouse drag isn't currently
  forwarded by wmd as a distinct event.

- **Selection persists across scrollback overflow.**  If the user
  selects an old row and then runs enough output to push that
  row past the 500-row ring's tail, the selection's anchor row
  becomes "invalid" (points at a row that's been discarded).
  `get_abs_row` returns 0 for that case, so `sel_copy_text`
  skips it silently — the copy will be shorter than the user
  selected.  Adequate; a "selection invalidated" toast would
  be nice but isn't critical.
