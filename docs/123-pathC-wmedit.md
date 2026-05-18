# Session 137 — Path C phase 30: wmedit text editor

**Goal.** A real text editor.  wmtype was a tiny scratchpad with
no file I/O and no cursor navigation; wmedit is the next step:
load a file, edit it, save it, copy/paste through the clipboard.

Status: **done.** Smoke test (`smoke_wmedit.py`, 6/6 pass):

```
=== pixel checks ===
  [OK] wmd-side title cyan @ y=209 (562/590)
  [OK] wmedit header blue @ y=222 (590/590)
  [OK] 'abc' rendered (63 green px)
  [OK] status bar bg @ (600,612) = (32, 40, 48)
  [OK] status bar text rendered (360)
  [OK] wmd status bar (877/924)
```

Boot wmd + `wmedit /tmp/smk 30`; cursor clicks into wmedit;
keyboard types "abc"; screendump confirms text is rendered in
the light-green editor body, the status bar at the bottom paints
in dark slate with the path + cursor position visible, and
wmd's outer chrome (cyan title from wmhello-style frame_color,
top status bar) is unbroken.

---

## What's in the editor

```
+---------------------------------------------+   ← wmd outer (CYAN frame)
| wmedit                            [_] [[] [X]|
+---------------------------------------------+
| wmedit  Ctrl-S save  Ctrl-Q quit             |  ← inner header (blue)
+---------------------------------------------+
|                                              |
|   abc_                                       |  ← body, light-green text
|                                              |  + blinking white caret
|   ...                                        |
|                                              |
+---------------------------------------------+
| /tmp/smk  [*]  L1:4 B3                       |  ← status bar (slate)
+---------------------------------------------+
   640 x 400
```

Static layout:
- Title bar (HDR_H = 18) — wmd's outer one is the standard
  WM chrome; the inner band below it is wmedit's "menu hint"
  showing the key shortcuts.
- Body — `(WIN_W - 2*GRID_X) / CELL_W = ~78` columns wide,
  `(WIN_H - HDR_H - FOOTER_H - 8) / LINE_H = ~36` rows tall.
- Footer (FOOTER_H = 14) — `<path> [*] L<row>:<col> B<bytes>`.
  The `[*]` marker shows in yellow only when `g_dirty` is set
  (i.e., edits exist that haven't been Ctrl+S'd).

Dynamic state:
- `g_buf[BUF_MAX = 8192]` — flat byte buffer with embedded `\n`
- `g_len` — bytes in buf
- `g_cur` — byte offset of the cursor
- `g_dirty` — modified-since-load/save flag
- `g_path[64]` — current file path
- `scroll_row` — top-of-viewport row (computed each frame so
  the cursor stays in view)

---

## Key bindings

| key                  | effect |
|----------------------|--------|
| printable ASCII      | insert at cursor |
| Enter                | insert `\n` |
| Backspace / DEL      | delete the byte before the cursor |
| Tab                  | insert 4 spaces |
| ↑ ↓ ← →             | move cursor by row/col |
| **Ctrl+S** (0x13)    | `sys_fs_write(g_path, g_buf, g_len)` |
| **Ctrl+Q** (0x11)    | exit cleanly |
| **Ctrl+C** (0x03)    | `wm_clipboard_set(g_buf, g_len)` (copy whole buffer) |
| **Ctrl+V** (0x16)    | `wm_clipboard_get` then `buf_insert` each byte |

Arrow-key parsing: the kernel injects arrows as ANSI CSI
sequences (ESC `[` final).  wmedit's `WM_EV_KEY` handler runs
a tiny 3-state machine:

```
esc_state = 0: byte 27 → state=1
esc_state = 1: byte '[' → state=2; anything else → fall through
esc_state = 2: byte A/B/C/D → cursor_move; reset state
```

Same shape as the wmfiles arrow handling from session 128.

---

## Cursor math

The buffer is flat bytes with embedded `\n`.  Row/col are
derived from a single byte offset:

```c
static void cursor_rowcol(int off, int *row, int *col) {
    int r = 0, c = 0;
    for (int i = 0; i < off; i++) {
        if (g_buf[i] == '\n') { r++; c = 0; }
        else                   { c++; }
    }
    *row = r; *col = c;
}
```

The inverse — "byte offset of the start of row N" — is also a
linear scan.  At 8 KiB this is fine; a serious editor would
keep a row-index sidecar that updates on edits.

Up-arrow tries to preserve the visual column:

```c
int new_col = col;
if (new_col > end - start) new_col = end - start;
g_cur = start + new_col;
```

If the previous row is shorter, the cursor lands at end-of-line.
If longer, it stays at the same column.

---

## What stays out of scope

- **Selection / copy-region.**  Ctrl+C copies the *whole*
  buffer, not a selection.  Selections need a (start, end)
  pair + visual feedback for the highlighted range; that's
  the next polish step.
- **Search / replace.**  No incremental search, no jump-to-
  line.  Would need a small input prompt overlay.
- **Undo.**  No edit history.  Single linear edit stream.
- **File picker.**  Path is fixed at launch time via argv[1].
  Future: prompt-style dialog over the body, or a stand-alone
  file-picker WM client that posts back via a new event.
- **Word-wrap.**  Lines longer than the viewport just clip;
  the cursor still moves through them by byte.
- **Auto-indent / syntax highlighting.**  Everything renders in
  one colour.

That's still enough to: launch wmedit on a file, navigate with
arrow keys, edit, Ctrl+S to save.  Cross-window copy/paste
works via the session-136 clipboard.

---

## Smoke-test design note

The 6th check splits the title bar into two pieces:
- `y=209` (wmd's outer title) → CYAN because that's the
  client's `frame_color` (set in `drain_wm_messages` to
  `GFX_CYAN`).
- `y=222` (wmedit's inner header bar) → blue 0x4080E0,
  wmedit's own paint.

Catching both confirms the WM/client boundary is correct: wmd
owns the CYAN frame, wmedit owns the blue inner band.

---

## Files touched

- `user/wmedit.c` — new, ~320 lines: flat-buffer editor with
  cursor, scroll, arrow-key parsing, Ctrl+S/Q/C/V handling,
  status bar
- `build.sh` — `wmedit` joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmedit.elf` + man page in the image
- `fs/man/wmedit` — new
- `user/wmd.c` — `wmedit` joins the Start-menu launcher catalog
  (now 10 items)
- `smoke_wmedit.py` — new harness, 6 pixel checks
- `docs/123-pathC-wmedit.md` — this file

kernel.bin: 143536 (unchanged — pure userspace change).
wmedit.bin: new 14548 bytes.  wmd.bin: 16440 → 16472 (+32 B
for the catalog entry).

---

## Path C status after session 137

- ✅ 107..136 — see prior docs
- ✅ 137 — wmedit text editor with file I/O + clipboard

The system now hosts a full editing workflow: open wmd → Start
→ wmterm to run shell commands AND wmedit to author files.  No
serial console needed.
