# Session 146 — Path C phase 39: resize from any edge / corner

**Goal.** Session 131 added a single 12×12 resize grip in the
bottom-right corner of every CLIENT window.  Functional but
limiting — once a window is in a corner of the screen you can't
grow it leftward without first moving it.

This session generalises the grip to a 5-zone hit-test: S, W, E
edges plus SW, SE corners.  Drag any of those and the window
resizes from that side.

Status: **done.**  Smoke `smoke_resize_edges.py` (4/4):

```
=== checks ===
  [OK] W-edge drag: left edge moved right (101 -> 151, diff 50)
  [OK] W-edge drag: right edge ~unchanged (742 -> 742, diff 0)
  [OK] S-edge drag: bottom moved down (619 -> 659, diff 40)
  [OK] wmd status bar alive (877/924)
```

Boot wmd + wmedit at outer (100, 200, 644, 420).  Drag the LEFT
edge right by 50 px: the left of the window moves from x=101 to
x=151 (exactly +50), the right stays at 742.  Drag the BOTTOM
down 40 px: bottom moves +40, top stays put.  Right-anchored
geometry is unchanged when W is dragged — proving the math
actually pivots on the opposite side.

---

## Hit-test layout

```
+--------- title bar (TITLE_H = 18) ---------+    <- top: drag-to-move
|                                            |       + min/max/close buttons
|  W                  E
|  ↕    (body — no zone)            ↕         |    <- W / E:  6 px wide
|                                            |       (only below title bar)
|                                            |
+--SW------------S------------SE-------------+    <- S:  6 px tall
                                                  -- SW / SE: 12×12 corners
```

- **N edge** and **NW / NE corners** deliberately don't resize.
  The title bar lives at the top and owns drag-to-move + the
  min/max/close button clicks.  Adding an N resize zone there
  would clash.
- **S edge**: bottom 6 px of the window.
- **W / E edges**: leftmost / rightmost 6 px, but **only below
  TITLE_H** so the title bar's hit handlers (drag, close, max,
  min) still win at the top.
- **SW / SE corners**: 12×12 boxes at the bottom-left and
  bottom-right.  Larger than the edges so corner-grabbing is
  forgiving.  SE is the original session-131 grip.

```c
static int in_resize_zone(struct window *w, int px, int py) {
    if (w->kind != KIND_CLIENT) return RES_NONE;
    int rx = px - w->x;
    int ry = py - w->y;
    if (rx < 0 || ry < 0 || rx >= w->w || ry >= w->h) return RES_NONE;

    int near_l = rx < RESIZE_BORDER;
    int near_r = rx >= w->w - RESIZE_BORDER;
    int near_b = ry >= w->h - RESIZE_BORDER;
    int in_corner_b = ry >= w->h - RESIZE_CORNER;

    if (in_corner_b && near_l) return RES_SW;
    if (in_corner_b && (rx >= w->w - RESIZE_CORNER || near_r)) return RES_SE;
    if (near_b)               return RES_S;
    if (ry < TITLE_H)         return RES_NONE;   /* title-bar owns top */
    if (near_l)               return RES_W;
    if (near_r)               return RES_E;
    return RES_NONE;
}
```

---

## Directional resize math

On mouse-press in a resize zone the WM stashes anchor state
(window x/y/w/h and the cursor position).  On every motion
event the new geometry is computed from the cursor delta,
honouring which sides should pivot:

```c
int dx = ms.x - g_resize_anchor_mx;
int dy = ms.y - g_resize_anchor_my;
int do_w = 0, do_e = 0, do_s = 0;
switch (g_resize_dir) {
    case RES_S:  do_s = 1; break;
    case RES_W:  do_w = 1; break;
    case RES_E:  do_e = 1; break;
    case RES_SW: do_w = 1; do_s = 1; break;
    case RES_SE: do_e = 1; do_s = 1; break;
}

if (do_e) new_w = anchor_w + dx;
if (do_w) {
    new_x = anchor_x + dx;
    new_w = anchor_w - dx;
}
if (do_s) new_h = anchor_h + dy;
```

The **W-drag clamp** is the subtle bit: if the new width would
drop below `WIN_MIN_W = 80`, the LEFT side should clamp at
`(anchor_x + anchor_w - WIN_MIN_W)` rather than `new_w` snapping
to 80 while x keeps tracking the cursor.  Without that, dragging
W aggressively to the right would push x past the window's
right edge and make the title bar fly off-screen.

---

## What stays out of scope

- **Top edge / NW / NE resize.**  The title bar lives at the top
  and is already busy with drag-to-move + close/max/min buttons.
  Allowing N resize there would mean adding a "drag from below
  the buttons but above the body" exclusion zone — fiddly and
  the visual indicator (cursor change) for hover wouldn't fit.
  All real WMs solve this with a separate top-edge zone above
  the title bar; AdventOS's window frame is title-bar-only so
  this isn't easy.
- **Cursor-shape feedback on hover.**  Standard WMs paint a
  ↔ cursor when hovering over a resize edge.  AdventOS has no
  guest cursor (session 142 removed it) — the QEMU host cursor
  is the only pointer, and we can't change its shape from the
  guest.  Users have to remember which 6-pixel border resizes.
- **Multi-monitor.**  Single FB still.

---

## Files touched

- `user/wmd.c`:
  - `RESIZE_BORDER` / `RESIZE_CORNER` constants and `RES_*`
    direction enum
  - `g_resize_dir`, `g_resize_anchor_x/y` added to resize state
  - `in_resize_zone()` — new 5-direction hit-test
  - `in_resize_grip()` — kept as a SE-only wrapper for any
    legacy call sites
  - Mouse-press handler — dispatches on `in_resize_zone()`
    return value
  - Motion handler — computes new x/y/w/h based on direction
  - Release handler — clears `g_resize_dir`
- `smoke_resize_edges.py` — new harness, 4 pixel checks
- `docs/132-pathC-resize-edges.md` — this file

Sizes:
- kernel.bin: 159920 (unchanged)
- wmd.bin: 18136 → 18620 (+484 B for the new hit-test + math)

---

## Path C status after session 146

- ✅ 107..145 — see prior docs
- ✅ 146 — resize from any edge / corner (S / W / E / SW / SE)
- ⚠️  wmterm input + close — still broken, deferred

Every CLIENT window can now be resized from the bottom edge,
either side, or the two bottom corners.  Pair with session-138
snap-to-edge for full keyboard-free window management.
