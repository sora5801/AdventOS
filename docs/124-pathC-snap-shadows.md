# Session 138 — Path C phase 31: snap-to-edge + drop shadows

**Goal.** Two pieces of pure WM polish, both wmd-side, no kernel
change:

1. **Drop shadows.** A 6-pixel soft gradient under and right of
   every window so layered windows read as having depth.
2. **Snap-to-edge.** Drag a window's title bar to a screen edge
   and on release it docks: top → maximize, left → fill-left-half,
   right → fill-right-half, bottom → fill-bottom-half.  Grabbing
   a snapped window's title bar un-snaps it back to its previous
   geometry.

Status: **done.**  Smoke test `smoke_wmsnap.py` (4/4):

```
=== pixel checks ===
  [OK] right shadow strip darkish (6/6)
  [OK] bottom shadow strip darkish (6/6)
  [OK] wmd status bar alive (877/924)
  [OK] wmedit title bar still painted
```

The shadow check reads the 6 pixels immediately right of and
below the wmedit window's edge and matches them against the
fixed gradient palette `sh[]` in `paint_window` — both strips
match exactly:

```
[0] (0, 3, 8)        ← darkest, closest to window
[1] (4, 8, 16)
[2] (6, 12, 22)
[3] (7, 14, 26)
[4] (8, 16, 32)
[5] (9, 21, 37)      ← lightest, blending into wallpaper
```

---

## Drop shadow design

Replaces the earlier hard 2-pixel black strip with a 6-strip
gradient.  No alpha blending — we just pick darker shades of
the wallpaper's blue band so the shadow reads as "depth" against
any of the eight gradient bands behind it.

```c
static const unsigned int sh[6] = {
    0x000308u, 0x040810u, 0x060C16u,
    0x070E1Au, 0x081020u, 0x091525u,
};
for (int i = 0; i < 6; i++) {
    unsigned int c = sh[i];
    int off = 2 + i;            /* displacement from window edge */
    gfx_fill_rect(ctx, w->x + w->w + i, w->y + off, 1, w->h, c);
    gfx_fill_rect(ctx, w->x + off, w->y + w->h + i, w->w, 1, c);
}
```

The shadow is offset by +(2..7) in each axis so the corner has a
2-pixel "light source from top-left" cue.  No top or left shadow
— the convention is a top-left-lit scene, same as macOS/Windows
defaults.

**Z-order matters.**  The shadow paints *first* (top of
`paint_window`) so the title bar / frame / content fill all sit
above it.  Otherwise the content fill would overwrite shadow
pixels that fall inside the window's own bounding box.  (None do,
since shadow strips are at `x = w->x + w->w + i` ≥ w->x + w->w —
strictly outside — but painting it first is correct anyway.)

---

## Snap-to-edge

Two halves:

### Snap on release

In the `if (released)` branch, just before clearing `g_drag_idx`,
we check if the cursor is within `SNAP_PX = 8` of any edge:

```c
if (g_drag_idx >= 0) {
    #define SNAP_PX 8
    struct window *w = &g_windows[g_drag_idx];
    int fb_w_i = (int)ctx.width;
    int fb_h_i = (int)ctx.height;
    int usable_top = 18;
    int usable_bot = fb_h_i - TASKBAR_H;
    int usable_h   = usable_bot - usable_top;
    int do_snap    = 0;
    int new_x, new_y, new_w, new_h;
    if (ms.y < usable_top + SNAP_PX) {
        new_x = 0; new_y = usable_top;
        new_w = fb_w_i; new_h = usable_h;
        do_snap = 1;
    } else if (ms.x < SNAP_PX) {
        new_x = 0; new_y = usable_top;
        new_w = fb_w_i / 2; new_h = usable_h;
        do_snap = 1;
    } else if (ms.x > fb_w_i - SNAP_PX) {
        new_x = fb_w_i / 2; new_y = usable_top;
        new_w = fb_w_i / 2; new_h = usable_h;
        do_snap = 1;
    } else if (ms.y > fb_h_i - TASKBAR_H - SNAP_PX) {
        new_x = 0; new_y = usable_top + usable_h / 2;
        new_w = fb_w_i; new_h = usable_h / 2;
        do_snap = 1;
    }
    if (do_snap && !w->maximized) {
        w->saved_x = w->x; w->saved_y = w->y;
        w->saved_w = w->w; w->saved_h = w->h;
        w->x = new_x; w->y = new_y;
        w->w = new_w; w->h = new_h;
        w->maximized = 1;
    }
}
```

The `maximized` flag is repurposed here — it already had the
"is currently filling a derived rect, with original geometry
saved" semantic from session 116's maximize button.  Snap just
reuses that infrastructure.  Caveat: it means you can't have
"maximized" and "snapped" as distinct states; un-snapping and
un-maximizing are the same code path.  Fine for now.

### Un-snap on title-bar grab

When you grab a snapped window's title bar, the WM restores the
original geometry first, placing the cursor at the same *relative*
horizontal position in the restored title bar:

```c
struct window *cw = &g_windows[hit];
if (cw->maximized) {
    int rel_x = ms.x - cw->x;
    int old_w = cw->w;
    if (old_w < 1) old_w = 1;
    cw->w = cw->saved_w;
    cw->h = cw->saved_h;
    cw->maximized = 0;
    cw->x = ms.x - (rel_x * cw->w / old_w);
    if (cw->x < 0) cw->x = 0;
    if (cw->x > (int)ctx.width - cw->w)
        cw->x = (int)ctx.width - cw->w;
    cw->y = ms.y - 5;
}
```

The scale factor `rel_x * cw->w / old_w` matters when the snapped
width is much larger than the restored width.  If you snapped to
the full screen and grabbed the title at x=800/1024, the cursor
is at 78% of the snapped title.  After restoring to (say) 640
wide, we want the cursor to still be at 78% of the new title →
x_in_window = 500.  Without the scale, the cursor would end up
*past* the right edge of the restored window and the drag would
feel detached.

---

## What's out of scope

- **Snap previews.**  A "ghost" outline showing where the window
  *would* snap before you release.  Common in Windows but adds
  per-frame state and a paint pass.  Skipped.
- **Quarter-snap.**  Dragging to a corner could do top-left /
  top-right / bottom-left / bottom-right quarter tiles.  The
  corners are currently dominated by edge proximity — top wins,
  no quarter zone.
- **Snap chord.**  Win+Arrow keyboard snap.  Could be added later
  by extending `wm_post_alttab` or a new SYS_WM_POLL_* channel.
- **Multi-monitor.**  No concept of monitors — there's one FB.
- **Animated transitions.**  Snap is instantaneous (one frame
  jump).  Would need frame interpolation in the main loop.

---

## Why the smoke doesn't actively test snap

QEMU's PS/2 rel-mouse delivers events with cursor-acceleration
scaling that varies across QEMU versions and host platforms.
Even with carefully tuned bursts, the guest cursor lands at an
unpredictable position.  Tests that rely on "drag the cursor to
exactly (x, y)" are flaky.

Earlier sessions (117 click-focus, 131 resize) work around this
by checking *what the WM did* after a chaotic input burst rather
than asserting a specific cursor position.  The shadow check
here follows the same principle — we know the shadow palette is
fixed and read it directly from the FB, no input needed.  For
snap-to-edge, the visual verification path is interactive:
launch `wmd` in QEMU with `-display gtk`, drag a window title to
each of the four edges.

The smoke still sends a chaotic drag burst as a
crash-resistance check (i.e., the snap code path doesn't fault
when exercised with unexpected coords); we just don't assert
"the window ended up snapped."

---

## Files touched

- `user/wmd.c`:
  - `paint_window` — new 6-strip shadow at the top, replacing the
    old 2-pixel black strip
  - main-loop `if (released)` branch — snap-on-release block
    (~35 lines)
  - main-loop title-bar-drag-start branch — un-snap block
    (~16 lines)
- `smoke_wmsnap.py` — new harness, 4 pixel checks
- `docs/124-pathC-snap-shadows.md` — this file

kernel.bin: 143536 (unchanged — pure userspace change).
wmd.bin: 16472 → 16920 bytes (+448 B for snap + shadow logic).

---

## Path C status after session 138

- ✅ 107..137 — see prior docs
- ✅ 138 — drop shadows + snap-to-edge

The WM chrome now has a soft depth cue under every window plus
modern-WM ergonomics for the common "put two things side by
side" gesture.  Both features are zero-cost on the kernel side
and add ~450 bytes to wmd.bin.
