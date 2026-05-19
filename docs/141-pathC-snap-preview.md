# Session 155 — Path C phase 48: snap-to-edge ghost preview

**Goal.** Polish session 138's snap-to-edge feature.  Currently a
window dragged near a screen edge SNAPS on release with no
warning — the user sees the snap happen as a "surprise."  This
session adds a 3-pixel-thick cyan outline at the snap target
during the drag, so the user can see exactly where the window
will land before letting go.

Status: **done.**  Smoke `smoke_snap_preview.py` (5/5):

```
=== checks ===
  [OK] top outline (984 cyan px)
  [OK] bottom outline (984 cyan px)
  [OK] left outline (700 cyan px)
  [OK] right outline (700 cyan px)
  [OK] wmd status bar alive (832/924)
```

Boot wmd + wmedit, press in wmedit's title bar, drag toward the
top edge so the cursor is within SNAP_PX (8) of y=18.  Mid-drag
screendump catches a thick cyan rectangle outlining the full
usable screen area below the top status bar and above the
taskbar — the destination of a top-snap.

---

## Refactor: shared snap-target helper

Both the release-time snap (session 138) and the new preview
needed the same logic — "given a cursor at (mx, my), is it in a
snap zone, and if so what rect should the window become?"
Factored into `snap_target_for`:

```c
#define SNAP_PX 8

static int snap_target_for(int fb_w_i, int fb_h_i, int mx, int my,
                           int *out_x, int *out_y,
                           int *out_w, int *out_h) {
    int usable_top = 18;
    int usable_bot = fb_h_i - TASKBAR_H;
    int usable_h   = usable_bot - usable_top;
    if (my < usable_top + SNAP_PX) {
        *out_x = 0; *out_y = usable_top;
        *out_w = fb_w_i; *out_h = usable_h;
        return 1;
    }
    if (mx < SNAP_PX) {
        *out_x = 0; *out_y = usable_top;
        *out_w = fb_w_i / 2; *out_h = usable_h;
        return 1;
    }
    /* ...mirror cases for E and S edges... */
    return 0;
}
```

The release-time path collapsed from ~40 lines of inline ifs to
a single call.  Net wmd.bin growth was only +384 B because the
release branch shrank while the preview added.

---

## Drawing the preview

Last paint step before `gfx_present` (so the outline is on top
of windows but below the host cursor):

```c
if (g_drag_idx >= 0) {
    int sx, sy, sw, sh;
    if (snap_target_for((int)ctx.width, (int)ctx.height,
                        ms.x, ms.y, &sx, &sy, &sw, &sh)) {
        unsigned int outline = 0x60D0F0u;   /* cyan */
        gfx_rect(&ctx, sx,     sy,     sw,     sh,     outline);
        gfx_rect(&ctx, sx + 1, sy + 1, sw - 2, sh - 2, outline);
        gfx_rect(&ctx, sx + 2, sy + 2, sw - 4, sh - 4, outline);
    }
}
```

Three concentric rectangles substitute for the alpha-blended
ghost an alpha-aware compositor would render — gives a
"chunky frame" look that's visible from across the screen.

Only paints when `g_drag_idx >= 0` (a title-bar drag is active)
AND the cursor is in a snap zone.  Move the cursor out of the
zone and the outline disappears; move back in and it returns.

---

## What stays out of scope

- **Diagonal corner snaps.**  Quarter-tile snap (top-left,
  top-right, etc) — current snap is 4-edge only.  Would need
  4 more zones and 4 more outline rects.
- **Translucent fill.**  No alpha blending; outline only.
- **Animated transition.**  Snap is instantaneous on release.
  An animated transition would need frame interpolation in
  the WM main loop.
- **Per-monitor.**  Single FB still.

---

## Files touched

- `user/wmd.c`:
  - `SNAP_PX` lifted from the inline `#define` inside the
    release branch to a file-scope macro
  - `snap_target_for(fb_w, fb_h, mx, my, *x, *y, *w, *h)`
    helper added
  - Release-time snap branch rewritten to call the helper
  - New per-frame preview paint at end of the main loop,
    drawing 3 concentric cyan outlines when a drag-snap is
    pending
- `smoke_snap_preview.py` — new harness, 5 pixel checks
- `docs/141-pathC-snap-preview.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged — pure userspace)
- wmd.bin: 20592 → 20976 (+384 B; the inline release branch
  shrank, the new helper + preview added, net only +384 B)

---

## Path C status after session 155

- ✅ 107..154 — see prior docs
- ✅ 155 — snap-to-edge ghost preview
- ⚠️  wmterm input + close — still deferred

Snap-to-edge is now the standard "drag to edge → preview →
release" gesture you'd expect from any modern WM.
