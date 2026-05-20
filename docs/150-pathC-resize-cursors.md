# Session 164 — resize-zone-aware cursor sprites

Path C phase 57.  Session 162 left this on the "not fixed yet"
list: "Resize-zone-aware cursor changes would be a natural
follow-up — wmd already detects resize zones for click handling,
so the data is right there; the renderer just needs a small
palette of sprites."  This session does that.

## What it does

Hover the pointer over a window's resize handle and the cursor
sprite changes:

| Hover zone | Cursor sprite       | Direction      |
|------------|---------------------|----------------|
| body       | arrow               | (default)      |
| W edge     | horizontal arrow    | `← →`          |
| E edge     | horizontal arrow    | `← →`          |
| S edge     | vertical arrow      | `↑ ↓`          |
| SW corner  | NE↔SW diagonal      | `↗ ↙`          |
| SE corner  | NW↔SE diagonal      | `↖ ↘`          |

The sprite stays the resize-cursor variant while a resize is
actively in progress, even if the cursor strays off the resize
zone mid-drag — that's the standard Windows / X11 behavior.

## How it's wired

The plumbing already existed.  `in_resize_zone(w, px, py)`
returns `RES_S / RES_W / RES_E / RES_SW / RES_SE` for the various
edges/corners, and `g_resize_dir` carries the active-resize
direction.  We just thread a `cursor_for_zone(zone)` helper into
the existing `draw_cursor()` call at the end of every frame:

```c
/* user/wmd.c — end-of-frame paint */
enum cursor_kind ck = CUR_ARROW;
if (g_resize_dir != RES_NONE) {
    ck = cursor_for_zone(g_resize_dir);
} else {
    int hit_for_cursor = hit_test(ms.x, ms.y);
    if (hit_for_cursor >= 0
        && g_windows[hit_for_cursor].kind == KIND_CLIENT) {
        int zone = in_resize_zone(&g_windows[hit_for_cursor],
                                  ms.x, ms.y);
        ck = cursor_for_zone(zone);
    }
}
draw_cursor(&ctx, ck, ms.x, ms.y);
```

## The sprite table

`draw_cursor` looks up the kind in `g_cursors[]`, a struct array
indexed by `enum cursor_kind`.  Each sprite carries its own
hot-spot offset (`hot_x`, `hot_y`) so we paint at
`(cursor_x - hot_x, cursor_y - hot_y)` and the hot-spot lands
exactly at the reported cursor coords no matter which shape is
active:

```c
struct cursor_spec {
    int hot_x, hot_y;
    int rows;
    const char *art[18];
};
```

Arrows: top-left tip is hot-spot `(0, 0)`, same as session 162.

Resize variants: center of the cross is hot-spot, so swapping
sprites doesn't make the cursor visually jump.

H_RESIZE is 16×7, V_RESIZE is 7×15, the diagonals are 13×13.
Each carries 1-2 embedded white-fill pixels (`.`) in the
arrowheads — necessary both visually (the shape stands out
against any background) and for the smoke (see below).

## Smoke

`smoke_wmd_resize_cursors.py` — six checks:

| Check                                              |
|----------------------------------------------------|
| cursor at W_edge differs from arrow                |
| cursor at E_edge differs from arrow                |
| cursor at S_edge differs from arrow                |
| cursor at SW_corner differs from arrow             |
| cursor at SE_corner differs from arrow             |
| W_edge cursor is horizontal (wider than tall)      |

The fingerprint approach: at each test position, find the cursor
centroid (black pixels that have a white-fill pixel within 2 px
— that filters out wmd's content-color fill which is also pure
black but contiguous with no whites), then encode the
black-pixel layout as a set of offsets from the centroid.  Two
fingerprints "differ" if their symmetric difference is > 4 px.

The aspect-ratio assertion is only on W_edge because the other
zones live too close to wmd's right/bottom content-fill black
bands (1-2 px between the client surface end and the window
frame).  Within the W_edge measurement, the wmd black band is
on the OPPOSITE side of the window so the bbox comes out clean.
The "differs from arrow" check covers the others robustly.

3/3 fresh-QEMU runs pass.  Sessions 161, 162 smokes still 100% —
no regression.

## What changed, exhaustively

- `user/wmd.c`:
  - `enum cursor_kind` + `struct cursor_spec` + `g_cursors[]`
    sprite table (5 entries).
  - `draw_cursor(ctx, kind, x, y)` walks the chosen sprite's art
    array and paints `#` black, `.` white, ` ` transparent at
    `(cursor_x - hot_x + dx, cursor_y - hot_y + dy)`.
  - `cursor_for_zone(zone)` maps `RES_*` to `CUR_*`.
  - Frame-paint hook picks the kind from the resize state (active
    resize → that variant; otherwise hit-test the hover position
    for a zone).
- `smoke_wmd_resize_cursors.py` — new.

## What this *doesn't* fix

- **Cursor doesn't change for the title-bar drag.**  Hovering the
  title bar still shows the arrow, even though clicking + dragging
  there moves the window.  A "move" hand-cursor variant would
  complete the cursor-shape palette; the data is there
  (`in_titlebar()`).

- **The wmd content-color fill behind a CLIENT window is still
  pure black.**  Visible as a 1-2 px dark seam between the client
  surface and the window frame outline.  Cosmetic; not visible
  unless the client's body color is much lighter than black.
  Fixing it cleanly probably means having wmd ask the client what
  background to use — not worth a session.

- **No I-beam cursor for text input.**  When wmterm has focus and
  the pointer is over its body, the cursor should arguably be an
  I-beam to signal "click here to position the caret".  But wmterm
  doesn't support clicking to position the caret yet (you type at
  the shell's read position), so an I-beam would be misleading
  today.
