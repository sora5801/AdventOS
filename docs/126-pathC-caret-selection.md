# Session 140 — Path C phase 33: wmedit caret + drag selection

**Goal.** Two text-editing polish features in wmedit:

1. **Vertical-line caret.** Replace the session-137 block caret
   (full-cell white rect) with a 2-pixel-wide vertical line.
   Still blinks on the same 12-tick phase (≈400 ms on / 400 ms
   off at 30 fps).
2. **Mouse drag selection.** Click in the body and drag to
   select a range; selected bytes get a blue background fill.
   Typing / Backspace / Ctrl-X / Ctrl-V operate on the selection
   when one is active; Ctrl-C copies the selection (or the
   whole buffer if none).

Status: **done.**  Smoke test `smoke_wmedit_sel.py` (4/4):

```
=== pixel checks ===
  [OK] typed text 'abcdef' rendered (129 green px)
  [OK] 2-px vertical caret seen
  [OK] selection highlight visible (232 blue px)
  [OK] wmd status bar alive (877/924)
```

Click into the body, type "abcdef", screendump, then drag from
near the start of the text rightward by ~4 cells and screendump
twice more.  The first sample catches the caret as a 2-px white
column at the end of "abcdef"; later samples catch the blue
selection rectangles painted under the dragged-over glyphs.

---

## State additions

```c
/* Session 140. */
static int g_sel_anchor = -1;   /* byte offset; -1 = no selection */
static int g_drag;              /* 1 while LMB held down */
static int g_scroll_row;        /* hoisted from main() locals */

static int sel_lo(void)     { return g_sel_anchor < g_cur ? g_sel_anchor : g_cur; }
static int sel_hi(void)     { return g_sel_anchor < g_cur ? g_cur : g_sel_anchor; }
static int sel_active(void) { return g_sel_anchor >= 0 && g_sel_anchor != g_cur; }
static void sel_clear(void) { g_sel_anchor = -1; }
```

The anchor is the byte offset where the mouse press landed; the
cursor (`g_cur`) tracks the latest drag position.  `sel_active()`
is true only when the two differ — a press without movement
collapses the selection on release.

The scroll position became a file-static (`g_scroll_row`) so the
new `byte_at_xy()` helper can compute click positions from event
handlers, which don't have access to main()'s locals.

---

## Event flow

```c
case WM_EV_MOUSE_PRESS: {
    int off = byte_at_xy(ev.x, ev.y);
    g_cur = off;
    g_sel_anchor = off;
    g_drag = 1;
    break;
}
case WM_EV_MOUSE_MOVE: {
    if (g_drag) g_cur = byte_at_xy(ev.x, ev.y);
    break;
}
case WM_EV_MOUSE_RELEASE: {
    g_drag = 0;
    if (g_cur == g_sel_anchor) g_sel_anchor = -1;
    break;
}
```

MOVE events fire continuously while the cursor is over the
client content area (wmd line 1119 only pushes a MOVE when
`ms.x != prev_mx || ms.y != prev_my`).  Dragging out of the
window stops the stream, but the last sample sticks — release
in any state still works.

`byte_at_xy` mirrors the click-to-cursor math that already
existed for MOUSE_PRESS, just factored out so MOVE can reuse it.

---

## Painting

Selection highlight is a `wm_fill_rect(0x305078)` painted *before*
the glyph for each selected cell, so the glyph renders on top
of the blue background:

```c
int slo = sel_active() ? sel_lo() : -1;
int shi = sel_active() ? sel_hi() : -1;
/* ... in the body paint loop ... */
if (byte >= slo && byte < shi) {
    wm_fill_rect(&win, GRID_X + col * CELL_W, y,
                 CELL_W, LINE_H, 0x305078u);
}
gfx_glyph(&sctx, GRID_X + col * CELL_W, y,
          g_buf[byte], 0xC0E0C0u, GFX_TRANSPARENT);
```

`gfx_glyph` with `GFX_TRANSPARENT` background paints only foreground
pixels, so the blue cell bg survives and the green text reads
fine on top.

Newlines in the selection get a one-cell highlight at end-of-row
so cross-row drags don't have a visual gap at line ends:

```c
if (byte < g_len && g_buf[byte] == '\n') {
    if (col < max_cols && byte >= slo && byte < shi) {
        wm_fill_rect(&win, GRID_X + col * CELL_W, y,
                     CELL_W, LINE_H, 0x305078u);
    }
    byte++;
}
```

The caret painter loses the block fill and gains a 2-px wide
strip, suppressed entirely while a selection is active (the
highlight already shows where the cursor end is):

```c
if (has_focus && !sel_active() && ((caret_phase / 12) & 1) == 0) {
    /* ... clip checks ... */
    wm_fill_rect(&win, cx, cy, 2, LINE_H - 1, 0xFFFFFFu);
}
```

---

## Selection-aware edits

Most edit paths now delete the active selection first:

| key                  | behaviour |
|----------------------|-----------|
| printable ASCII      | sel_delete() + buf_insert(k) |
| Enter                | sel_delete() + buf_insert('\n') |
| Tab                  | sel_delete() + 4× buf_insert(' ') |
| Backspace / DEL      | sel_delete() if active, else buf_delete() |
| Ctrl-C               | copy [sel_lo, sel_hi) if active, else whole buffer |
| **Ctrl-X (new)**     | copy + sel_delete (cut) — no-op without selection |
| Ctrl-V               | sel_delete() + insert each clipboard byte |
| Arrow keys           | sel_clear() (no range mutation) |

`sel_delete` shifts the trailing bytes down, decrements `g_len`,
places the cursor at the deleted range's lower bound, and clears
the anchor.  Edge cases (`g_sel_anchor == g_cur`) are caught by
`sel_active()` returning 0.

---

## Caret design choice

Block caret (session 137):  one full 8×8 white cell.  Easy to
see, but obscures the underlying glyph.  Made it hard to tell
*which* glyph the cursor was at.

Vertical line (session 140):  2 px wide × `LINE_H-1 = 9 px`
tall, at the *left edge* of the cursor's column.  Doesn't
overlap the previous glyph or the next glyph; reads as
"insertion point between two characters."  Standard for almost
every text editor since the original Mac.

2 px (not 1) so it stays visible against light backgrounds —
1 px white on a 0xC0E0C0 green-grey body can disappear at the
wrong eye/monitor settings.

---

## What stays out of scope

- **Shift+arrow keyboard selection.**  Currently selection is
  mouse-only.  The kernel doesn't surface modifier state on the
  ANSI CSI passthrough — arrow keys come through as raw CSI
  letters with no Shift indicator.  Adding modifier propagation
  would be a kernel + libwm + every-client refactor.
- **Double-click word select / triple-click line select.**  No
  click-count tracking yet.
- **Drag-out scroll.**  Dragging past the bottom of the body
  should auto-scroll; currently the cursor just clamps to the
  last visible row.
- **Search highlighting.**  No find-mode at all yet (session
  137 noted that too).
- **Undo for cut operations.**  Cut deletes the selection
  permanently with no history.

The dropdown of mouse-driven select + clipboard cut/copy/paste
is enough to round-trip text within a single wmedit session and
across windows via the session-136 clipboard.

---

## Files touched

- `user/wmedit.c`:
  - selection state + helpers (`sel_lo/hi/active/clear`,
    `sel_delete`, `byte_at_xy`)
  - hoisted `scroll_row` to file-static `g_scroll_row`
  - MOUSE_PRESS → start drag; MOUSE_MOVE → extend; MOUSE_RELEASE
    → collapse-if-empty
  - paint: selection-bg fill before glyph; vertical-line caret
    (and suppressed while selection active)
  - key handlers: sel-aware typing / backspace / Ctrl-C / Ctrl-V;
    new Ctrl-X cut; arrow keys clear selection
- `fs/man/wmedit` — updated to document the new behaviours
- `smoke_wmedit_sel.py` — new harness, 4 pixel checks
- `docs/126-pathC-caret-selection.md` — this file

kernel.bin: 147632 (unchanged — pure userspace change).
wmedit.bin: 14548 → 15480 bytes (+932 B for selection state,
helpers, and selection-aware edit paths).

---

## Path C status after session 140

- ✅ 107..139 — see prior docs
- ✅ 140 — caret vertical line + drag selection in wmedit

wmedit is now the right shape for a serious editor: visible
insertion-point caret, drag-to-select with visual highlight,
clipboard-aware cut.  Still missing search, undo, modifier-aware
keyboard select — natural Path C polish targets going forward.
